/********************************************************************************************************\

	File Name:	OracleSkirmish.h
	Descript:	Replay oracle: a fixed two-ship skirmish that logs per-sample ship state.

\********************************************************************************************************/

#if !defined( OracleSkirmish_h )
#define OracleSkirmish_h

#include <string>
#include "ScriptInterface.h"

class tOracleRecorder;

class tOracleSkirmish : public tScript
{
public:

	tOracleSkirmish();
	virtual ~tOracleSkirmish();

	virtual void	mStartMission();
	virtual int32	mInitializeStart( tDynaverseScriptInfo& info );
	virtual void	mCreateExternalShips( int32 Mode );
	virtual int32	mInitializeFinished();
	virtual void	mNewInternalShip( tShipInfo* Ship );
	virtual void	mScriptSpecs( tMissionSpec* Specs );
	virtual void	mNewTeam( const tTeamScriptDescription& teamScriptDescription );
	virtual void	mASCIIOverrides( tMapOverrides* Map );

protected:

	tOracleRecorder*	fRecorder;
	std::string			fSeedReport;
};

#endif
