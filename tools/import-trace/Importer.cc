#include <sstream>
#include <iostream>
#include "Importer.hpp"
#include "util/Logger.hpp"

using namespace fail;

static Logger LOG("Importer");

bool Importer::init(const std::string &variant, const std::string &benchmark, Database *db) {
	this->db = db;
	m_variant_id = db->get_variant_id(variant, benchmark);
	if (!m_variant_id) {
		return false;
	}
	LOG << "Importing to variant " << variant << "/" << benchmark
		<< " (ID: " << m_variant_id << ")" << std::endl;
	return true;
}

bool Importer::clear_database() {
	std::stringstream ss;
	ss << "DELETE FROM trace WHERE variant_id = " << m_variant_id;

	bool ret = db->query(ss.str().c_str()) == 0 ? false : true;
	LOG << "deleted " << db->affected_rows() << " rows from trace table" << std::endl;
	return ret;
}

bool Importer::copy_to_database(fail::ProtoIStream &ps) {
	unsigned row_count = 0, row_count_fake = 0;
	// map for keeping one "open" EC for every address
	// (maps injection data address =>
	// dyn. instruction count / time information for equivalence class left margin)
	AddrLastaccessMap open_ecs;

	// time the trace started/ended
	// For now we just use the min/max occuring timestamp; for "sparse" traces
	// (e.g., only mem accesses, only a subset of the address space) it might
	// be a good idea to store this explicitly in the trace file, though.
	simtime_t time_trace_start = 0, curtime = 0;

	// instruction counter within trace
	instruction_count_t instr = 0;

	Trace_Event ev;

	while (ps.getNext(&ev)) {
		if (ev.has_time_delta()) {
			// record trace start
			// it suffices to do this once, the events come in sorted
			if (time_trace_start == 0) {
				time_trace_start = ev.time_delta();
				LOG << "trace start time: " << time_trace_start << std::endl;
			}
			// curtime also always holds the max time, provided we only get
			// nonnegative deltas
			assert(ev.time_delta() >= 0);
			curtime += ev.time_delta();
		}

		// instruction events just get counted
		if (!ev.has_memaddr()) {
			// new instruction
			// sanity check for overflow
			if (instr == (1LL << (sizeof(instr)*8)) - 1) {
				LOG << "error: instruction_count_t overflow, aborting at instr=" << instr << std::endl;
				return false;
			}
			instr++;
			continue;
		}

		address_t from = ev.memaddr(), to = ev.memaddr() + ev.width();
		// Iterate over all accessed bytes
		// FIXME Keep complete trace information (access width)?
		//   advantages: may be used for pruning strategies, complete value would be visible; less DB entries
		//   disadvantages: may need splitting when width varies, lots of special case handling
		//   Probably implement this in a separate importer when necessary.
		for (address_t data_address = from; data_address < to; ++data_address) {
			// skip events outside a possibly supplied memory map
			if (m_mm && !m_mm->isMatching(data_address)) {
				continue;
			}
			instruction_count_t instr1 =
				open_ecs[data_address].dyninstr; // defaults to 0 if nonexistent
			instruction_count_t instr2 = instr; // the current instruction
			simtime_t time1 = open_ecs[data_address].time;
			// defaulting to 0 is not such a good idea, memory reads at the
			// beginning of the trace would get an unnaturally high weight:
			if (time1 == 0) {
				time1 = time_trace_start;
			}
			simtime_t time2 = curtime;

			// skip zero-sized intervals: these can occur when an instruction
			// accesses a memory location more than once (e.g., INC, CMPXCHG)
			// FIXME: look at timing instead?
			if (instr1 > instr2) {
				continue;
			}

			ev.set_width(1);
			ev.set_memaddr(data_address);

			// we now have an interval-terminating R/W event to the memaddr
			// we're currently looking at; the EC is defined by
			// data_address, dynamic instruction start/end, the absolute PC at
			// the end, and time start/end
			if (!add_trace_event(instr1, instr2, time1, time2, ev)) {
				LOG << "add_trace_event failed" << std::endl;
				return false;
			}
			row_count ++;
			if (row_count % 10000 == 0) {
				LOG << "Inserted " << row_count << " trace events into the database" << std::endl;
			}

			// next interval must start at next instruction; the aforementioned
			// skipping mechanism wouldn't work otherwise
			//lastuse_it->second = instr2 + 1;
			open_ecs[data_address].dyninstr = instr2 + 1;
			open_ecs[data_address].time = time2 + 1;
		}
	}

	// Close all open intervals (right end of the fault-space) with fake trace
	// event.  This ensures we have a rectangular fault space (important for,
	// e.g., calculating the total SDC rate), and unknown memory accesses after
	// the end of the trace are properly taken into account: Either with a
	// "don't care" (a synthetic memory write at the right margin), or a "care"
	// (synthetic read), the latter resulting in more experiments to be done.
	for (AddrLastaccessMap::iterator lastuse_it = open_ecs.begin();
		lastuse_it != open_ecs.end(); ++lastuse_it) {

		Trace_Event fake_ev;
		fake_ev.set_memaddr(lastuse_it->first);
		fake_ev.set_width(1);
		fake_ev.set_accesstype(m_faultspace_rightmargin == 'R' ? fake_ev.READ : fake_ev.WRITE);

		instruction_count_t instr1 = lastuse_it->second.dyninstr;
		simtime_t time1 = lastuse_it->second.time;

		// Why -1?	In most cases it does not make sense to inject before the
		// very last instruction, as we won't execute it anymore.  This *only*
		// makes sense if we also inject into parts of the result vector.  This
		// is not the case in this experiment, and with -1 we'll get a result
		// comparable to the non-pruned campaign.
		instruction_count_t instr2 = instr - 1;

		simtime_t time2 = curtime; // -1?

// #else
//		// EcosKernelTestCampaign only variant: fault space ends with the last FI experiment
//		FIXME probably implement this with cmdline parameter FAULTSPACE_CUTOFF
//		int instr2 = instr_rightmost;
// #endif

		// zero-sized?	skip.
		// FIXME: look at timing instead?
		if (instr1 > instr2) {
			continue;
		}

		if (!add_trace_event(instr1, instr2, time1, time2, fake_ev, true)) {
			LOG << "add_trace_event failed" << std::endl;
			return false;
		}
		++row_count_fake;
	}

	LOG << "trace duration: " << (curtime - time_trace_start) << " ticks" << std::endl;
	LOG << "Inserted " << row_count << " trace events (+" << row_count_fake
	    << " fake events) into the database" << std::endl;

	// TODO: (configurable) sanity checks
	// PC-based fault space rectangular, covered, and non-overlapping?
	// (same for timing-based fault space?)

	return true;
}
