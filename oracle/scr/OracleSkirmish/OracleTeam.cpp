/********************************************************************************************************\

	File Name:	OracleTeam.cpp
	Descript:	Team 1 gets ORACLE_SHIP_TEAM1 on map letter G, team 2 gets ORACLE_SHIP_TEAM2 on map letter H.
				No start position offset (kNoShipOptions), so placement does not consume random numbers.

\********************************************************************************************************/

#include "tclient.h"
#include "MissionText.h"
#include "ShipScriptDescription.h"
#include "OracleConfig.h"
#include "OracleTeam.h"

tClassAllocator<tOracleTeam> gOracleTeamAllocator;

void tOracleTeam::mSetupDebriefingList( eVictoryLevels victoryLevel )
{
	fMissionInfo->mAddToDebriefingList( fTeam, kDefault_msg );
}

void tOracleTeam::mCreateShipsForTeam()
{
	if( fTeam == kTeam1 )
	{
		mCreateShip( typeid( tShipInfo ), ORACLE_SHIP_TEAM1, kStartPosition_G, kNoMetaShipID,
					 kOracleShipTeam1, 0, 0, -1, -1, NULL, kNoShipOptions );
	}
	else if( fTeam == kTeam2 )
	{
		mCreateShip( typeid( tShipInfo ), ORACLE_SHIP_TEAM2, kStartPosition_H, kNoMetaShipID,
					 kOracleShipTeam2, 0, 0, -1, -1, NULL, kNoShipOptions );
	}
}

/*
	The parent must be called first: it registers the team and its ships with the event manager.
*/
void tOracleTeam::mTeamSetupComplete()
{
	tTeamInfo::mTeamSetupComplete();

	tShipIterator ship = mGetFirstShip();
	if( *ship != NULL && mIsTeamHumanControlled() )
	{
		ship->mSetShipPlayerControlled();
#if ORACLE_PLAYER_AUTOPILOT
		ship->mSetShipAIControlled( kTrue );
#endif
	}
}

/*
	Ships picked on the setup screen are ignored; mCreateShipsForTeam() creates the fixed ones.
*/
void tOracleTeam::mCreateNewShip( const tShipScriptDescription& shipScriptDescription )
{}
