#pragma once

#include "CoreMinimal.h"
#include "Command/ChatCommandInstance.h"
#include "BJAutopilotCommand.generated.h"

/**
 * /autopilot off|on [filter] — the chat twin of BJ.Autopilot.
 *
 * SML intercepts a chat line starting with "/" on the client and sends it to the server through
 * its own remote call object, where the command subsystem runs it. So any player can use this
 * from a client of a dedicated server, and the client needs only SML, not this mod. The reply
 * goes back the same way and shows up in the chat as a SYSTEM message.
 */
UCLASS()
class ABJAutopilotCommand : public AChatCommandInstance
{
	GENERATED_BODY()
public:
	ABJAutopilotCommand();
	EExecutionStatus ExecuteCommand_Implementation(UCommandSender* Sender, const TArray<FString>& Arguments, const FString& Label) override;
};
