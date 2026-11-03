/********************************************************************************************************\

	File Name:	OracleConfig.h
	Descript:	Every fixed input of the replay oracle in one place.  Changing anything here changes the
				trace, so bump kOracleTraceVersion whenever a value changes.

\********************************************************************************************************/

#if !defined( OracleConfig_h )
#define OracleConfig_h

#include "tbasic.h"

//
//	Trace format version, written to the header.  Bump on any change to this file or to the record layout.
//
const int32 kOracleTraceVersion = 1;

//
//	Trace file, written next to StarFleetOP.exe.  The driver waits for the "END" record in it.
//
#define ORACLE_TRACE_FILE			"OracleTrace.txt"

//
//	Sampling.  kTimerTurnsSet runs on game turn time, not wall-clock seconds, so the sample points are
//	tied to the simulation.  1/32 of a turn is one impulse if the engine honours fractional turns; the
//	trace header records the value so a mismatch is visible.
//
const float kOracleSampleTurns		=	1.0f / 32.0f;
const int32 kOracleMaxSamples		=	32 * 20;	// 20 turns
const int32 kOracleQuietSamples		=	32;			// keep sampling this long after one side is gone
const int32 kOracleTimerID			=	7001;

//
//	Ships: one fixed spec record per team, placed on fixed map letters with no random start offset.
//	Records are from Assets/Specs/shiplist.txt (2.5.5.2): F-CA (Federation heavy cruiser, BPV 125) and
//	K-D7 (Klingon heavy cruiser, BPV 126).
//
#define ORACLE_SHIP_TEAM1			"F-CA"
#define ORACLE_SHIP_TEAM2			"K-D7"

enum eOracleCustomIDs
{
	kOracleShipTeam1 = 101,
	kOracleShipTeam2 = 201,
};

//
//	1: the human's ship is flown by the AI (autopilot), so no input is needed and both ships fight.
//	0: the human's ship stays under (absent) human control and sits still as a target.
//
#define ORACLE_PLAYER_AUTOPILOT		1

//
//	1: pin the game's two random number generators (see OracleSeed.cpp) at mission start and at
//	sample 0.  0: leave them alone (useful to measure how much the trace varies without pinning).
//
#define ORACLE_PIN_RNG				1
const unsigned int kOracleCrtSeed	=	0x5FC0A001;
const int32 kOracleLcgSeed			=	1;			// the LCG's own static initial value in 2.5.5.2

#endif
