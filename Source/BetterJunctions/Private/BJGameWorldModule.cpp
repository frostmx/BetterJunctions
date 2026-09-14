#include "BJGameWorldModule.h"

#include "BJAutopilotCommand.h"

UBJGameWorldModule::UBJGameWorldModule()
{
	bRootModule = true;
	mChatCommands.Add(ABJAutopilotCommand::StaticClass());
}
