#ifndef __ARM_ARCHITECURE_HPP__
  #define __ARM_ARCHITECURE_HPP__

#include "../CPU.hpp"
#include "../CPUState.hpp"

namespace fail {

/**
 * \class ArmArchitecture
 * This class adds ARM specific functionality to the base architecture.
 * This can be used for every simulator backend that runs on ARM.
 */
class ArmArchitecture : public CPUArchitecture {
public:
	ArmArchitecture();
	~ArmArchitecture();
};

/**
 * \enum GPRegIndex
 * Defines the general purpose (GP) register identifier for the ARM
 * plattform. Some of them are just aliases.
 */
enum GPRegIndex {
	RI_R0,
	RI_R1,
	RI_R2,
	RI_R3,
	RI_R4,
	RI_R5,
	RI_R6,
	RI_R7,
	RI_R8,
	RI_R9,
	RI_R10,
	RI_R11,
	RI_R12,
	RI_R13,
	RI_SP = RI_R13,
	RI_R14,
	RI_LR = RI_R14,
	RI_R15,
	RI_IP = RI_R15,

	RI_R13_SVC,
	RI_R14_SVC,

	RI_R13_MON,
	RI_R14_MON,

	RI_R13_ABT,
	RI_R14_ABT,

	RI_R13_UND,
	RI_R14_UND,

	RI_R13_IRQ,
	RI_R14_IRQ,

	RI_R8_FIQ,
	RI_R9_FIQ,
	RI_R10_FIQ,
	RI_R11_FIQ,
	RI_R12_FIQ,
	RI_R13_FIQ,
	RI_R14_FIQ
};

// TODO: Enum for misc registers, see (e.g.)
//       simulators/gem5/src/arch/arm/miscregs.hh and
//       simulators/gem5/src/arch/arm/intregs.hh

} // end-of-namespace: fail

#endif // __ARM_ARCH_HPP__