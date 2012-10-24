#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <algorithm>
#include <cmath>

#include "campaign.hpp"
#include "experimentInfo.hpp"
#include "cpn/CampaignManager.hpp"
#include "util/Logger.hpp"
#include "util/ProtoStream.hpp"

#include "UDIS86.hpp"
#include "udis86_helper.hpp"

#include "../plugins/tracing/TracingPlugin.hpp"

using namespace std;
using namespace fail;

bool get_file_contents(const char *filename, string& contents)
{
	std::ifstream in(filename, std::ios::in | std::ios::binary);
	if (!in) {
		return false;
	}
	in.seekg(0, std::ios::end);
	contents.resize(in.tellg());
	in.seekg(0, std::ios::beg);
	in.read(&contents[0], contents.size());
	in.close();
	return true;
}

bool NanoJPEGCampaign::run()
{
	init_results();

	// load binary image (objcopy'ed system.elf = system.bin)
	string binimage;
	if (!get_file_contents(NANOJPEG_BIN, binimage)) {
		m_log << "couldn't open " << NANOJPEG_BIN << endl;
		return false;
	}
	Udis86 udis(0);
	Udis86Helper udis_helper;
	Udis86Helper::UDRegisterSet in_regs, out_regs;

	// register cascade for equivalence class generation
	// map: register -> list of latest accesses (front = newest access)
	// list: latest accesses (instr offset | bit mask)
	map<GPRegisterId, std::list<std::pair<unsigned, uint64_t> > > reg_cascade;
	// open up an equivalence class for all bits in all GPRs
	reg_cascade[RID_EAX].push_front(std::pair<unsigned, uint64_t>(0, 0xffffffffffffffffULL));
	reg_cascade[RID_EBX].push_front(std::pair<unsigned, uint64_t>(0, 0xffffffffffffffffULL));
	reg_cascade[RID_ECX].push_front(std::pair<unsigned, uint64_t>(0, 0xffffffffffffffffULL));
	reg_cascade[RID_EDX].push_front(std::pair<unsigned, uint64_t>(0, 0xffffffffffffffffULL));
	reg_cascade[RID_ESP].push_front(std::pair<unsigned, uint64_t>(0, 0xffffffffffffffffULL));
	reg_cascade[RID_EBP].push_front(std::pair<unsigned, uint64_t>(0, 0xffffffffffffffffULL));
	reg_cascade[RID_ESI].push_front(std::pair<unsigned, uint64_t>(0, 0xffffffffffffffffULL));
	reg_cascade[RID_EDI].push_front(std::pair<unsigned, uint64_t>(0, 0xffffffffffffffffULL));

	// load trace
	ifstream tracef(NANOJPEG_TRACE);
	if (tracef.fail()) {
		m_log << "couldn't open " << NANOJPEG_TRACE << endl;
		return false;
	}
	ProtoIStream ps(&tracef);

	// experiment count
	int count_exp = 0;
	int count_exp_jobs = 0;
	// known output count
	int count_known = 0;
	int count_known_jobs = 0;

	// instruction counter within trace
	unsigned instr = 0;

	Trace_Event ev;
	// for every event in the trace ...
	while (ps.getNext(&ev) && instr < NANOJPEG_INSTR_LIMIT) {
		// sanity check: skip memory access entries
		if (ev.has_memaddr()) {
			continue;
		}
		instr++;

		// disassemble instruction
		if (ev.ip() < NANOJPEG_BIN_OFFSET || ev.ip() - NANOJPEG_BIN_OFFSET > binimage.size()) {
			m_log << "traced IP 0x" << hex << ev.ip() << " outside system image" << endl;
			continue;
		}
		udis.setIP(ev.ip());
		size_t ip_offset = ev.ip() - NANOJPEG_BIN_OFFSET;
		udis.setInputBuffer((unsigned char *) &binimage[ip_offset], min((size_t) 20, binimage.size() - ip_offset));

		if (!udis.fetchNextInstruction()) {
			m_log << "fatal: cannot disassemble instruction at 0x" << hex << ev.ip() << endl;
			return false;
		}

		ud_t& ud = udis.getCurrentState();
		udis_helper.setUd(&ud);

		//m_log << "0x" << hex << ev.ip() << " " << ::ud_insn_asm(&ud) << endl;
		//for (int i = 0; i < 3; ++i) {
		//	cout << udis_helper.operandTypeToChar(ud.operand[i].type) << " ";
		//}
		//cout << endl;

		// determine input/output registers
		udis_helper.inOutRegisters(in_regs, out_regs);
#if 0
		cout << "IN: ";
		for (Udis86Helper::UDRegisterSet::const_iterator it = in_regs.begin();
		     it != in_regs.end(); ++it) {
			cout << udis_helper.typeToString(*it) << " ";
		}
		cout << "OUT: ";
		for (Udis86Helper::UDRegisterSet::const_iterator it = out_regs.begin();
		     it != out_regs.end(); ++it) {
			cout << udis_helper.typeToString(*it) << " ";
		}
		cout << endl;
#endif

		// all IN registers close an equivalence class and generate experiments
		for (Udis86Helper::UDRegisterSet::const_iterator it = in_regs.begin();
		     it != in_regs.end(); ++it) {
			// determine Fail* register ID and bitmask
			uint64_t access_mask;
			fail::GPRegisterId reg = udis_helper.udisGPRToFailBochsGPR(*it, access_mask);
			uint64_t remaining_access_mask = access_mask;

			// iterate over latest accesses to register, newest to oldest
			for (std::list<std::pair<unsigned, uint64_t> >::iterator acc = reg_cascade[reg].begin();
			     acc != reg_cascade[reg].end() && remaining_access_mask; ) {
				uint64_t curr_mask = acc->second;
				uint64_t common_mask = curr_mask & remaining_access_mask;
				if (!common_mask) {
					++acc;
					continue;
				}

				remaining_access_mask &= ~common_mask;
				acc->second &= ~common_mask;

				// new EC with experiments: acc->first -- instr, common_mask
//				if (reg != RID_EBP && reg != RID_ESI && reg != RID_EDI) {
					count_exp += add_experiment_ec(acc->first, instr, ev.ip(), reg, common_mask);
					count_exp_jobs++;
//				}

				// new memory access EC in access cascade
				reg_cascade[reg].push_front(std::pair<unsigned, uint64_t>(instr + 1, common_mask));

				// old access completely shadowed by newer accesses?
				if (acc->second == 0) {
					acc = reg_cascade[reg].erase(acc);
				} else {
					++acc;
				}
			}
			if (remaining_access_mask != 0) {
				m_log << "something weird happened: remaining_access_mask = 0x" << hex << remaining_access_mask << endl;
			}
		}

		// all OUT registers close an equivalence class and generate known results
		// special case: empty EC!
		for (Udis86Helper::UDRegisterSet::const_iterator it = out_regs.begin();
		     it != out_regs.end(); ++it) {
			// determine Fail* register ID and bitmask
			uint64_t access_mask;
			fail::GPRegisterId reg = udis_helper.udisGPRToFailBochsGPR(*it, access_mask);
			uint64_t remaining_access_mask = access_mask;

			// iterate over latest accesses to register, newest to oldest
			for (std::list<std::pair<unsigned, uint64_t> >::iterator acc = reg_cascade[reg].begin();
			     acc != reg_cascade[reg].end() && remaining_access_mask; ) {
				uint64_t curr_mask = acc->second;
				uint64_t common_mask = curr_mask & remaining_access_mask;
				if (!common_mask) {
					++acc;
					continue;
				}

				remaining_access_mask &= ~common_mask;
				acc->second &= ~common_mask;

				// skip empty EC (because register was read within the same instruction)?
				if (acc->first <= instr) {
					// new EC with known result: acc->first -- instr, common_mask
//					if (reg != RID_EBP && reg != RID_ESI && reg != RID_EDI) {
						count_known += add_known_ec(acc->first, instr, ev.ip(), reg, common_mask);
						count_known_jobs++;
//					}
				}

				// new memory access EC in access cascade
				reg_cascade[reg].push_front(std::pair<unsigned, uint64_t>(instr + 1, common_mask));

				// old access completely shadowed by newer accesses?
				if (acc->second == 0) {
					acc = reg_cascade[reg].erase(acc);
				} else {
					++acc;
				}
			}
			if (remaining_access_mask != 0) {
				m_log << "something weird happened: remaining_access_mask = 0x" << hex << remaining_access_mask << endl;
			}
		}
	}
	campaignmanager.noMoreParameters();
	cout << "experiments planned: " << dec << count_exp
	     << " (" << count_exp_jobs << " jobs)" << endl;
	cout << "known outcome ECs: " << dec << count_known
	     << " (" << count_known_jobs << " jobs)" << endl;

	// collect results
	NanoJPEGExperimentData *res;
	int rescount = 0;
	while ((res = static_cast<NanoJPEGExperimentData *>(campaignmanager.getDone()))) {
		rescount++;

		NanoJPEGProtoMsg_Result const *prev_singleres = 0;
		uint64_t bitmask = 0;

		// one job contains multiple experiments
		for (int idx = 0; idx < res->msg.result_size(); ++idx) {
			NanoJPEGProtoMsg_Result const *cur_singleres = &res->msg.result(idx);
			if (!prev_singleres) {
				prev_singleres = cur_singleres;
				bitmask = 1ULL << cur_singleres->bitnr();
				continue;
			}
			// compatible?  merge.
			if (prev_singleres->resulttype() == cur_singleres->resulttype()
			 && prev_singleres->latest_ip() == cur_singleres->latest_ip()
			 && prev_singleres->psnr() == cur_singleres->psnr()
			 && prev_singleres->details() == cur_singleres->details()) {
				bitmask |= 1ULL << cur_singleres->bitnr();
				continue;
			}

			add_result(res->msg.instr_ecstart(), res->msg.instr_offset(),
				res->msg.instr_address(), (fail::GPRegisterId) res->msg.register_id(),
				res->msg.timeout(), res->msg.injection_ip(),
				bitmask, prev_singleres->resulttype(), prev_singleres->latest_ip(),
				prev_singleres->psnr(), prev_singleres->details().c_str());

			prev_singleres = cur_singleres;
			bitmask = 1ULL << cur_singleres->bitnr();
		}
		add_result(res->msg.instr_ecstart(), res->msg.instr_offset(),
			res->msg.instr_address(), (fail::GPRegisterId) res->msg.register_id(),
			res->msg.timeout(), res->msg.injection_ip(),
			bitmask, prev_singleres->resulttype(), prev_singleres->latest_ip(),
			prev_singleres->psnr(), prev_singleres->details().c_str());
		//delete res;	// currently racy if jobs are reassigned
	}

	finalize_results();
	return true;
}

int NanoJPEGCampaign::add_experiment_ec(unsigned instr_ecstart, unsigned instr_offset,
	unsigned instr_address, fail::GPRegisterId register_id, uint64_t bitmask)
{
	uint64_t v = bitmask; // count the number of bits set in v
	int c; // c accumulates the total bits set in v

	for (c = 0; v; v >>= 1) {
		c += v & 1;
	}

	// TODO check result CSV if we already know the answer

	// enqueue job
	NanoJPEGExperimentData *d = new NanoJPEGExperimentData;
	d->msg.set_instr_ecstart(instr_ecstart);
	d->msg.set_instr_offset(instr_offset);
	d->msg.set_instr_address(instr_address);
	d->msg.set_register_id(register_id);
	d->msg.set_bitmask(bitmask);
	d->msg.set_timeout(NANOJPEG_TIMEOUT);
	campaignmanager.addParam(d);

	return c;
}

int NanoJPEGCampaign::add_known_ec(unsigned instr_ecstart, unsigned instr_offset,
	unsigned instr_address, fail::GPRegisterId register_id, uint64_t bitmask)
{
	uint64_t v = bitmask; // count the number of bits set in v
	int c; // c accumulates the total bits set in v

	for (c = 0; v; v >>= 1) {
		c += v & 1;
	}

	// TODO check result CSV if we already stored the answer

	add_result(instr_ecstart, instr_offset, instr_address, register_id, 0, 0,
		bitmask, NanoJPEGProtoMsg_Result::FINISHED, 0, INFINITY, "");
	return c;
}

bool NanoJPEGCampaign::init_results()
{
	// read already existing results
	bool file_exists = false;
/*
	set<uint64_t> existing_results;
	ifstream oldresults(results_filename, ios::in);
	if (oldresults.is_open()) {
		char buf[16*1024];
		uint64_t addr;
		int count = 0;
		m_log << "scanning existing results ..." << endl;
		file_exists = true;
		while (oldresults.getline(buf, sizeof(buf)).good()) {
			stringstream ss;
			ss << buf;
			ss >> addr;
			if (ss.fail()) {
				continue;
			}
			++count;
			if (!existing_results.insert(addr).second) {
				m_log << "duplicate: " << addr << endl;
			}
		}
		m_log << "found " << dec << count << " existing results" << endl;
		oldresults.close();
	}
*/

	// non-destructive: due to the CSV header we can always manually recover
	// from an accident (append mode)
	resultstream.open(NANOJPEG_RESULTS, ios::out | ios::app);
	if (!resultstream.is_open()) {
		m_log << "failed to open " << NANOJPEG_RESULTS << endl;
		return false;
	}
	// only write CSV header if file didn't exist before
	if (!file_exists) {
		resultstream << "instr_ecstart\tinstr_offset\tinstr_address\tregister_id\ttimeout\tinjection_ip\tbitmask\tresulttype\tlatest_ip\tpsnr\tdetails" << endl;
	}
	return true;
}

void NanoJPEGCampaign::add_result(unsigned instr_ecstart,
	unsigned instr_offset, uint32_t instr_address, fail::GPRegisterId register_id,
	unsigned timeout, uint32_t injection_ip, uint64_t bitmask, int resulttype,
	uint32_t latest_ip, float psnr, char const *details)
{
	resultstream << hex
		<< instr_ecstart << "\t"
		<< instr_offset << "\t"
		<< instr_address << "\t"
		<< register_id << "\t"
		<< timeout << "\t"
		<< injection_ip << "\t"
		<< bitmask << "\t"
		<< resulttype << "\t"
		<< latest_ip << "\t"
		<< dec << psnr << "\t"
		<< details << "\n";
}

void NanoJPEGCampaign::finalize_results()
{
	resultstream.close();
}
