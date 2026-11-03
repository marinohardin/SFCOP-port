/********************************************************************************************************\

	File Name:	MissionText.cpp
	Descript:	Text for the oracle mission.  The title starts with '!' so it sorts first in the mission
				list, which keeps the driver's click sequence short.

\********************************************************************************************************/

#include "tclient.h"
#include "MissionText.h"

std::string Messages[] =
{
	//	kMissionTitle_msg,
	"!Oracle Skirmish",

	//	kDescription_msg,
	"Replay oracle for the SFCOP port.\n" \
	"F-CA vs K-D7, both AI, fixed positions.\n" \
	"Writes OracleTrace.txt.\n\n",

	//	kBriefing_msg,
	"",

	//	kHelpTags_msg,
	"",

	//	kStart_msg,
	"",

	//	kYouAreDead_msg,
	"",

	//	kWinner_msg,
	"Winner!",

	//	kLoser_msg,
	"Loser!",

	//	kLeftEarly_msg,
	"Disconnect.\n\n",

	//	kDefault_msg,
	"Oracle run complete.\n\n",
};
