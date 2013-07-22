#include "util/CommandLine.hpp"
#include "util/Database.hpp"
#include "util/ElfReader.hpp"
#include "util/MemoryMap.hpp"
#include "util/gzstream/gzstream.h"
#include "util/Logger.hpp"
#include <fstream>
#include <string>
#include "MemoryImporter.hpp"

#ifdef BUILD_LLVM_DISASSEMBLER
#include "InstructionImporter.hpp"
#include "RegisterImporter.hpp"
#include "RandomJumpImporter.hpp"
#endif


using namespace fail;
using std::cerr;
using std::endl;
using std::cout;

static Logger LOG("import-trace", true);

ProtoIStream openProtoStream(std::string input_file) {
	std::ifstream *gz_test_ptr = new std::ifstream(input_file.c_str()), &gz_test = *gz_test_ptr;
	if (!gz_test) {
		LOG << "couldn't open " << input_file << endl;
		exit(-1);
	}
	unsigned char b1, b2;
	gz_test >> b1 >> b2;

	if (b1 == 0x1f && b2 == 0x8b) {
		igzstream *tracef = new igzstream(input_file.c_str());
		if (!tracef) {
			LOG << "couldn't open " << input_file << endl;
			exit(-1);
		}
		LOG << "opened file " << input_file << " in GZip mode" << endl;
		delete gz_test_ptr;
		ProtoIStream ps(tracef);
		return ps;
	}

	LOG << "opened file " << input_file << " in normal mode" << endl;
	ProtoIStream ps(gz_test_ptr);
	return ps;
}

int main(int argc, char *argv[]) {
	std::string trace_file, username, hostname, database, benchmark;
	std::string variant;
	ElfReader *elf_file = 0;
	MemoryMap *memorymap = 0;

	// Manually fill the command line option parser
	CommandLine &cmd = CommandLine::Inst();
	for (int i = 1; i < argc; ++i)
		cmd.add_args(argv[i]);

	cmd.addOption("", "", Arg::None, "USAGE: import-trace [options]");
	CommandLine::option_handle HELP =
		cmd.addOption("h", "help", Arg::None, "-h/--help \tPrint usage and exit");
	CommandLine::option_handle TRACE_FILE =
		cmd.addOption("t", "trace-file", Arg::Required,
			"-t/--trace-file \tFile to load the execution trace from\n");

	// setup the database command line options
	Database::cmdline_setup();

	CommandLine::option_handle VARIANT =
		cmd.addOption("v", "variant", Arg::Required,
			"-v/--variant \tVariant label (default: \"none\")");
	CommandLine::option_handle BENCHMARK =
		cmd.addOption("b", "benchmark", Arg::Required,
			"-b/--benchmark \tBenchmark label (default: \"none\")\n");
	CommandLine::option_handle IMPORTER =
		cmd.addOption("i", "importer", Arg::Required,
			"-i/--importer \tWhich import method to use (default: MemoryImporter)");
	CommandLine::option_handle ELF_FILE =
		cmd.addOption("e", "elf-file", Arg::Required,
			"-e/--elf-file \tELF File (default: UNSET)");
	CommandLine::option_handle MEMORYMAP =
		cmd.addOption("m", "memorymap", Arg::Required,
			"-m/--memorymap \tMemory map to intersect with trace (may be used more than once; default: UNSET)");
	CommandLine::option_handle NO_DELETE =
		cmd.addOption("", "no-delete", Arg::None,
			"--no-delete \tAssume there are no DB entries for this variant/benchmark, don't issue a DELETE");


	// variant 1: care (synthetic Rs)
	// variant 2: don't care (synthetic Ws)
	CommandLine::option_handle FAULTSPACE_RIGHTMARGIN =
		cmd.addOption("", "faultspace-rightmargin", Arg::Required,
			"--faultspace-rightmargin \tMemory access type (R or W) to "
			"complete fault space at the right margin "
			"(default: W -- don't care)");
	// (don't) cutoff at first R
	// (don't) cutoff at last R
	//CommandLine::option_handle FAULTSPACE_CUTOFF =
	//	cmd.addOption("", "faultspace-cutoff-end", Arg::Required,
	//		"--faultspace-cutoff-end \tCut off fault space end (no, lastr) "
	//		"(default: no)");

	CommandLine::option_handle ENABLE_SANITYCHECKS =
		cmd.addOption("", "enable-sanitychecks", Arg::None,
			"--enable-sanitychecks \tEnable sanity checks "
			"(in case something looks fishy)"
			"(default: disabled)");

	if (!cmd.parse()) {
		std::cerr << "Error parsing arguments." << std::endl;
		exit(-1);
	}

	Importer *importer;

	if (cmd[IMPORTER].count() > 0) {
		std::string imp(cmd[IMPORTER].first()->arg);
		if (imp == "BasicImporter" || imp == "MemoryImporter" || imp == "memory" || imp == "mem") {
			LOG << "Using MemoryImporter" << endl;
			importer = new MemoryImporter();
#ifdef BUILD_LLVM_DISASSEMBLER
		} else if (imp == "InstructionImporter" || imp == "code") {
			LOG << "Using InstructionImporter" << endl;
			importer = new InstructionImporter();

		} else if (imp == "RegisterImporter" || imp == "regs") {
			LOG << "Using RegisterImporter" << endl;
			importer = new RegisterImporter();

		} else if (imp == "RandomJumpImporter") {
			LOG << "Using RandomJumpImporter" << endl;
			importer = new RandomJumpImporter();
#endif
		} else {
			LOG << "Unkown import method: " << imp << endl;
			exit(-1);
		}

	} else {
		LOG << "Using MemoryImporter" << endl;
		importer = new MemoryImporter();
	}

	if (importer && !(importer->cb_commandline_init())) {
		std::cerr << "Cannot call importers command line initialization!" << std::endl;
		exit(-1);
	}

	if (cmd[HELP]) {
		// Since the importer might have added command line options,
		// we need to reparse all arguments in order to prevent a
		// segfault within optionparser
		cmd.parse();
		cmd.printUsage();
		exit(0);
	}

	if (cmd[TRACE_FILE].count() > 0)
		trace_file = std::string(cmd[TRACE_FILE].first()->arg);
	else
		trace_file = "trace.pb";

	ProtoIStream ps = openProtoStream(trace_file);
	Database *db = Database::cmdline_connect();

	if (cmd[VARIANT].count() > 0)
		variant = std::string(cmd[VARIANT].first()->arg);
	else
		variant = "none";

	if (cmd[BENCHMARK].count() > 0)
		benchmark = std::string(cmd[BENCHMARK].first()->arg);
	else
		benchmark = "none";

	if (cmd[ELF_FILE].count() > 0) {
		elf_file = new ElfReader(cmd[ELF_FILE].first()->arg);
	}
	importer->set_elf(elf_file);

	if (cmd[MEMORYMAP].count() > 0) {
		memorymap = new MemoryMap();
		for (option::Option *o = cmd[MEMORYMAP]; o; o = o->next()) {
			if (!memorymap->readFromFile(o->arg)) {
				LOG << "failed to load memorymap " << o->arg << endl;
			}
		}
	}
	importer->set_memorymap(memorymap);

	if (cmd[FAULTSPACE_RIGHTMARGIN].count() > 0) {
		std::string rightmargin(cmd[FAULTSPACE_RIGHTMARGIN].first()->arg);
		if (rightmargin == "W") {
			importer->set_faultspace_rightmargin('W');
		} else if (rightmargin == "R") {
			importer->set_faultspace_rightmargin('R');
		} else {
			LOG << "unknown memory access type '" << rightmargin << "', using default" << endl;
			importer->set_faultspace_rightmargin('W');
		}
	} else {
		importer->set_faultspace_rightmargin('W');
	}

	if (cmd[ENABLE_SANITYCHECKS].count() > 0) {
		importer->set_sanitychecks(true);
	}

	if (!importer->init(variant, benchmark, db)) {
		LOG << "importer->init() failed" << endl;
		exit(-1);
	}

	////////////////////////////////////////////////////////////////
	// Do the actual import
	////////////////////////////////////////////////////////////////

	if (!importer->create_database()) {
		LOG << "create_database() failed" << endl;
		exit(-1);
	}

	if (cmd[NO_DELETE].count() == 0 && !importer->clear_database()) {
		LOG << "clear_database() failed" << endl;
		exit(-1);
	}

	if (!importer->copy_to_database(ps)) {
		LOG << "copy_to_database() failed" << endl;
		exit(-1);
	}
}
