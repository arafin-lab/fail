#include <iostream>

#include <unistd.h>

#include "experiment.hpp"
#include "SAL/SALInst.hpp"
#include "SAL/bochs/BochsRegister.hpp"
#include "controller/Event.hpp"
#include "util/Logger.hpp"
#include "config/FailConfig.hpp"

// Check if configuration dependencies are satisfied:
#if !defined(CONFIG_EVENT_BREAKPOINTS) || !defined(CONFIG_SR_RESTORE) || !defined(CONFIG_SR_SAVE)
  #error This experiment needs: breakpoints, save, and restore. Enable these in the configuration.
#endif

using std::endl;

bool hscsimpleExperiment::run()
{
	Logger log("HSC", false);
	log << "experiment start" << endl;

	// do funny things here...
#if 1
    // STEP 1
	fi::BPSingleEvent mainbp(0x00003c34);
	sal::simulator.addEventAndWait(&mainbp);
	log << "breakpoint reached, saving" << endl;
	sal::simulator.save("hello.state");
#elif 0
    // STEP 2
	log << "restoring ..." << endl;
	sal::simulator.restore("hello.state");
	log << "restored!" << endl;

	log << "waiting for last square() instruction" << endl;
	fi::BPSingleEvent breakpoint(0x3c9e); // square(x) ret instruction
	sal::simulator.addEventAndWait(&breakpoint);
	log << "injecting hellish fault" << endl;
	// RID_CAX is the RAX register in 64 bit mode and EAX in 32 bit mode:
	sal::simulator.getRegisterManager().getRegister(sal::RID_CAX)->setData(666);
	log << "waiting for last main() instruction" << endl;
	breakpoint.setWatchInstructionPointer(0x3c92);
	sal::simulator.addEventAndWait(&breakpoint);

	log << "reached" << endl;

	sal::simulator.addEventAndWait(&breakpoint);
#endif

	sal::simulator.clearEvents(this);
	return true;
}
