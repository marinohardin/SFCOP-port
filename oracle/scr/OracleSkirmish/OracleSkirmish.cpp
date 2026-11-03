/********************************************************************************************************\

	Mission:	OracleSkirmish
	Script:		"OracleSkirmish"
	Descript:	Replay oracle for the SFCOP port.  One fixed ship per side on a fixed empty map, both
				flown by the AI, random number generators pinned at mission start.  tOracleRecorder
				writes OracleTrace.txt next to the game executable.  See oracle/README.md.

\********************************************************************************************************/

#include "MissionText.h"
#include "MissionMaps.h"
#include "OracleConfig.h"
#include "OracleRecorder.h"
#include "OracleSeed.h"
#include "OracleTeam.h"
#include "OracleSkirmish.h"

DEFINE_SCRIPT_CLASS( tOracleSkirmish );

tOracleSkirmish::tOracleSkirmish() :
	fRecorder( NULL )
{}

/*
	The recorder is registered with gEventMgr, which deletes it; do not delete it here.
*/
tOracleSkirmish::~tOracleSkirmish()
{}

/*
	Two teams: team 1 is the human's slot (its ship is flown by the AI, see ORACLE_PLAYER_AUTOPILOT),
	team 2 is AI only.  Both are always present (chance 1, required).
*/
void tOracleSkirmish::mScriptSpecs( tMissionSpec* Specs )
{
	Specs->fMissionTitle = Messages[kMissionTitle_msg];
	Specs->fMissionDescription = Messages[kDescription_msg];
	Specs->fHelpTags = Messages[kHelpTags_msg];
	Specs->fMissionType = kSkirmishMission;
	Specs->fPlayerRace = kSpecAnyRace;
	Specs->fNumShips = kMissSpecOneShip;
	Specs->fMenuStates = kDisableCustomSkirmish | kDisableAllTags;	// ships and sides are fixed by the script
	Specs->fBandwidthPart = static_cast< eBandwidthPart >( k1Band | k5Client );

	const tTeamSpec& team1 = mCreateTeamSpec(	"Oracle A",
												kTeam1,
												kMaxChance,
												kTrue,
												kPlayableTeam,
												kMinChance,
												kSpecPlayableRace,
												kAnyRace,
												typeid( tOracleTeam ),
												kTagA );

	const tTeamSpec& team2 = mCreateTeamSpec(	"Oracle B",
												kTeam2,
												kMaxChance,
												kTrue,
												kNPCTeam,
												kMaxChance,
												kSpecPlayableRace,
												kAnyRace,
												typeid( tOracleTeam ),
												kTagB );

	Specs->mSetupTeam( team1 );
	Specs->mSetupTeam( team2 );
}

/*
	Only used for kUniqueCampaignMission.  With TotalMaps == 1 every other mission type gets map 0 too.
*/
int32 tOracleSkirmish::mInitializeStart( tDynaverseScriptInfo& info )
{
	return 1;
}

void tOracleSkirmish::mCreateExternalShips( int32 Mode )
{}

int32 tOracleSkirmish::mInitializeFinished( void )
{
	return kBriefing_msg;
}

void tOracleSkirmish::mNewInternalShip( tShipInfo* Ship )
{}

void tOracleSkirmish::mStartMission()
{
	tScript::mStartMission();

	const int kRelationsCount = 2;
	int32 tagList[kRelationsCount] =
	{
		kTagA,
		kTagB,
	};
	std::string relationships[kRelationsCount][kRelationsCount] =
	{
		//		 A     B
		/*A   */ "AI", "WD",
		/*B   */ "WD", "AI",
	};
	fMissionInfo->mSetTagRelations( tagList, ( std::string* )relationships, kRelationsCount );

#if ORACLE_PIN_RNG
	fSeedReport = nOracleSeed::mPin( kOracleCrtSeed, kOracleLcgSeed );
#else
	fSeedReport = "off";
#endif

	fRecorder = new tOracleRecorder();
	gEventMgr.mRegisterActor( fRecorder );
	fRecorder->mStart( fSeedReport );
}

void tOracleSkirmish::mNewTeam( const tTeamScriptDescription& teamScriptDescription )
{
	tScript::mNewTeam( teamScriptDescription );
}

void tOracleSkirmish::mASCIIOverrides( tMapOverrides* Map )
{
	tScript::mASCIIOverrides( Map );

	Map->fTacticalMapIcon['G'] = kObjectPlayerShip;
	Map->fTacticalMapIcon['H'] = kObjectPlayerShip;
}
