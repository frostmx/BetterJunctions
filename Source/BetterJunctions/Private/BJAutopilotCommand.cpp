#include "BJAutopilotCommand.h"

#include "BJHooks.h"
#include "Command/CommandSender.h"

ABJAutopilotCommand::ABJAutopilotCommand()
{
	CommandName = TEXT("autopilot");
	// SML prints Usage itself when fewer arguments arrive.
	MinNumberOfArguments = 1;
	Usage = NSLOCTEXT("BetterJunctions", "ChatCommand.Autopilot.Usage",
		"/autopilot off|on [name filter] - switch autopilot for every truck at once");
}

EExecutionStatus ABJAutopilotCommand::ExecuteCommand_Implementation(UCommandSender* Sender, const TArray<FString>& Arguments, const FString& Label)
{
	bool bEnable = false;
	if (Arguments.Num() == 0 || !FBetterJunctionsHooks::ParseAutopilotMode(Arguments[0], bEnable))
	{
		PrintCommandUsage(Sender);
		return EExecutionStatus::BAD_ARGUMENTS;
	}
	// Per-truck lines would flood the chat: they go to LogBetterJunctions only (Say logs without
	// a console), the chat gets the summary.
	const FString Summary = FBetterJunctionsHooks::SetAutopilotForAll(GetWorld(), bEnable,
		Arguments.Num() > 1 ? Arguments[1] : FString(), nullptr);
	Sender->SendChatMessage(Summary);
	return EExecutionStatus::COMPLETED;
}
