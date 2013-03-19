#include <iostream>
#include <fstream>

#include "sal/SALInst.hpp"
#include "sal/Register.hpp"
#include "sal/Listener.hpp"
#include "experiment.hpp"
#include "util/CommandLine.hpp"
#include "util/gzstream/gzstream.h"

// You need to have the tracing plugin enabled for this
#include "../plugins/tracing/TracingPlugin.hpp"


/*
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/gzip_stream.h>
*/

using namespace std;
using namespace fail;

void  GenericTracing::parseOptions() {
    CommandLine &cmd = CommandLine::Inst();
    CommandLine::option_handle IGNORE = cmd.addOption("", "", Arg::None, "USAGE: fail-client -Wf,[option] -Wf,[option] ... <BochsOptions...>\n\n");
    CommandLine::option_handle HELP = cmd.addOption("h", "help", Arg::None, "-h,--help\t Print usage and exit");
    CommandLine::option_handle ELF_FILE = cmd.addOption("", "elf-file", Arg::Required,
                                                            "--elf-file\t ELF Binary File (default: $FAIL_ELF_PATH)");
    CommandLine::option_handle START_SYMBOL = cmd.addOption("s", "start-symbol", Arg::Required,
                                                            "-s,--start-symbol\t ELF symbol to start tracing (default: main)");
    CommandLine::option_handle STOP_SYMBOL  = cmd.addOption("e", "end-symbol", Arg::Required,
                                                            "-e,--end-symbol\t ELF symbol to end tracing");
    CommandLine::option_handle SAVE_SYMBOL  = cmd.addOption("S", "save-symbol", Arg::Required,
                                                            "-S,--save-symbol\t ELF symbol to save the state of the machine (default: main)\n");
    CommandLine::option_handle STATE_FILE   = cmd.addOption("f", "state-file", Arg::Required,
                                                            "-f,--state-file\t File/dir to save the state to (default state)");
    CommandLine::option_handle TRACE_FILE   = cmd.addOption("t", "trace-file", Arg::Required,
                                                            "-t,--trace-file\t File to save the execution trace to\n");

    CommandLine::option_handle MEM_SYMBOL   = cmd.addOption("m", "memory-symbol", Arg::Required,
                                                            "-m,--memory-symbol\t ELF symbol(s) to trace accesses (without specifiying all mem read/writes are traced)");
    CommandLine::option_handle MEM_REGION   = cmd.addOption("M", "memory-region", Arg::Required,
                                                            "-M,--memory-region\t restrict memory region which is traced"
                                                            "   Possible formats: 0x<address>, 0x<address>:0x<address>, 0x<address>:<length>");

    if(!cmd.parse()) {
        cerr << "Error parsing arguments." << endl;
        exit(-1);
    }

    if (cmd[HELP]) {
        cmd.printUsage();
        exit(0);
    }

    if (cmd[ELF_FILE].count() > 0)
        elf_file = cmd[ELF_FILE].first()->arg;
    else {
        char * elfpath = getenv("FAIL_ELF_PATH");
        if(elfpath == NULL){
            m_log << "FAIL_ELF_PATH not set :( (alternative: --elf-file) " << std::endl;
            exit(-1);
        }

        elf_file = elfpath;
    }
    m_elf = new ElfReader(elf_file.c_str());

    if (cmd[START_SYMBOL].count() > 0)
        start_symbol = cmd[START_SYMBOL].first()->arg;
    else
        start_symbol = "main";

    if (cmd[STOP_SYMBOL].count() > 0)
        stop_symbol = std::string(cmd[STOP_SYMBOL].first()->arg);
    else {
        m_log << "You have to give an end symbol (-e,--end-symbol)!" << std::endl;
        exit(EXIT_FAILURE);
    }

    if (cmd[SAVE_SYMBOL].count() > 0)
        save_symbol = std::string(cmd[SAVE_SYMBOL].first()->arg);
    else
        save_symbol = "main";

    if (cmd[STATE_FILE].count() > 0)
        state_file = std::string(cmd[STATE_FILE].first()->arg);
    else
        state_file = "state";

    if (cmd[TRACE_FILE].count() > 0)
        trace_file = std::string(cmd[TRACE_FILE].first()->arg);
    else
        trace_file = "trace.pb";

    use_memory_map = false;

    if (cmd[MEM_SYMBOL].count() > 0) {
        use_memory_map = true;
        option::Option *opt = cmd[MEM_SYMBOL].first();

        while(opt != 0) {
            const ElfSymbol &symbol = m_elf->getSymbol(opt->arg);
            assert(symbol.isValid());

            m_log << "Adding '" << opt->arg << "' == 0x" << std::hex << symbol.getAddress()
                  << "+" << std::dec << symbol.getSize() << " to trace map" << std::endl;
            traced_memory_map.add(symbol.getAddress(), symbol.getSize());

            opt = opt->next();
        }
    }

    if (cmd[MEM_REGION].count() > 0) {
        use_memory_map = true;
        option::Option *opt = cmd[MEM_REGION].first();

        while(opt != 0) {
            char *endptr;
            guest_address_t begin = strtol(opt->arg, &endptr, 16);
            guest_address_t size;
            if (endptr == opt->arg) {
                m_log << "Couldn't parse " << opt->arg << std::endl;
                exit(-1);
            }

            char delim = *endptr;
            if (delim == 0) {
                size = 1;
            } else if(delim == ':') {
                char *p = endptr +1;
                size = strtol(p, &endptr, 16) - begin;
                if (p == endptr || *endptr != 0) {
                    m_log << "Couldn't parse " << opt->arg << std::endl;
                    exit(-1);
                }
            } else if(delim == '+') {
                char *p = endptr +1;
                size = strtol(p, &endptr, 10);
                if (p == endptr || *endptr != 0) {
                    m_log << "Couldn't parse " << opt->arg << std::endl;
                    exit(-1);
                }
            } else {
                m_log << "Couldn't parse " << opt->arg << std::endl;
                exit(-1);
            }

            traced_memory_map.add(begin, size);

            m_log << "Adding " << opt->arg << " 0x" << std::hex << begin
                  << "+" << std::dec << size << " to trace map" << std::endl;

            opt = opt->next();
        }
    }

    assert(m_elf->getSymbol(start_symbol).isValid());
    assert(m_elf->getSymbol(stop_symbol).isValid());
    assert(m_elf->getSymbol(save_symbol).isValid());

    m_log << "start symbol: " << start_symbol << " 0x" << std::hex << m_elf->getSymbol(start_symbol).getAddress() << std::endl;
    m_log << "save symbol: "  << save_symbol  << " 0x" << std::hex << m_elf->getSymbol(save_symbol).getAddress() << std::endl;
    m_log << "stop symbol: "  << stop_symbol  << " 0x" << std::hex << m_elf->getSymbol(stop_symbol).getAddress() << std::endl;

    m_log << "state file: "   << state_file << std::endl;
    m_log << "trace file: "   << trace_file << std::endl;

}

bool GenericTracing::run()
{
    parseOptions();

    BPSingleListener l_start_symbol(m_elf->getSymbol(start_symbol).getAddress());
    BPSingleListener l_save_symbol (m_elf->getSymbol(save_symbol).getAddress());
    BPSingleListener l_stop_symbol (m_elf->getSymbol(stop_symbol).getAddress());

    ////////////////////////////////////////////////////////////////
    // STEP 1: run until interesting function starts, start the tracing
    simulator.addListenerAndResume(&l_start_symbol);
    m_log << start_symbol << " reached, start tracing" << std::endl;

    // restrict memory access logging to injection target
    TracingPlugin tp;

    if (use_memory_map) {
        m_log << "Use restricted memory map for tracing" << std::endl;
        tp.restrictMemoryAddresses(&traced_memory_map);
    }

    ogzstream of(trace_file.c_str());
    if (of.bad()) {
        m_log << "Couldn't open trace file: " << trace_file << std::endl;
        exit(-1);
    }
    tp.setTraceFile(&of);

    // this must be done *after* configuring the plugin:
    simulator.addFlow(&tp);

    ////////////////////////////////////////////////////////////////
    // STEP 2: continue to the save point, and save state
    if (start_symbol != save_symbol) {
        simulator.addListenerAndResume(&l_save_symbol);
    }
    m_log << start_symbol << " reached, save state" << std::endl;
    simulator.save(state_file);


    ////////////////////////////////////////////////////////////////
    // Step 3: Continue to the stop point
    simulator.addListener(&l_stop_symbol);
    simulator.resume();

    ////////////////////////////////////////////////////////////////
    // Step 4: tear down the tracing

	simulator.removeFlow(&tp);
	// serialize trace to file
	if (of.fail()) {
		m_log << "failed to write " << trace_file << std::endl;
		return false;
	}
    of.close();

	simulator.clearListeners();
	simulator.terminate();

	return true;
}