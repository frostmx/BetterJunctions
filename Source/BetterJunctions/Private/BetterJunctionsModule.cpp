#include "BetterJunctions.h"

#include "BJHooks.h"

DEFINE_LOG_CATEGORY(LogBetterJunctions);

IMPLEMENT_MODULE(FBetterJunctionsModule, BetterJunctions);

void FBetterJunctionsModule::StartupModule()
{
#if WITH_EDITOR
	// Never hook in an editor or commandlet process: FactoryGame is built there from SDK stubs
	// with empty bodies, and funchook aborts the process on them ("Too short instructions"),
	// which kills the cook.
	UE_LOG(LogBetterJunctions, Display, TEXT("editor build: hooks not installed"));
#else
	FBetterJunctionsHooks::Install();
#endif
}

void FBetterJunctionsModule::ShutdownModule()
{
}
