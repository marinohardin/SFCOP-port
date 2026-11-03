/********************************************************************************************************\

	File Name:	MissionText.h
	Descript:	Text for the oracle mission.

\********************************************************************************************************/

#if !defined( MissionText_h )
#define MissionText_h

#include "tstring.h"

enum TextMessages
{
	kMissionTitle_msg,
	kDescription_msg,
	kBriefing_msg,
	kHelpTags_msg,
	kStart_msg,
	kYouAreDead_msg,
	kWinner_msg,
	kLoser_msg,
	kLeftEarly_msg,
	kDefault_msg,
};

extern std::string Messages[];

#endif
