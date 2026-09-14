#pragma once

#include "CoreMinimal.h"
#include "Module/GameWorldModule.h"
#include "BJGameWorldModule.generated.h"

/**
 * Registers the /autopilot chat command with SML.
 *
 * SML discovers native root modules by class (FPluginModuleLoader::FindRootModulesOfType via
 * FindNativeClassesByType), so a code-only mod needs no content asset for this, only
 * bRootModule on the CDO. Chat commands are registered on the server side only, which is the
 * only side this mod runs on anyway.
 */
UCLASS()
class UBJGameWorldModule : public UGameWorldModule
{
	GENERATED_BODY()
public:
	UBJGameWorldModule();
};
