/********************************************************************************************************\

	File Name:	OracleTeam.h
	Descript:	Both oracle teams.  Ships are fixed by the script, never taken from the setup screen.

\********************************************************************************************************/

#if !defined( OracleTeam_h )
#define OracleTeam_h

#include "TeamInfo.h"

class tOracleTeam : public tTeamInfo
{
public:

	virtual void mSetupDebriefingList( eVictoryLevels victoryLevel );
	virtual void mCreateShipsForTeam();
	virtual void mTeamSetupComplete();
	virtual void mCreateNewShip( const tShipScriptDescription& shipScriptDescription );
};

#endif
