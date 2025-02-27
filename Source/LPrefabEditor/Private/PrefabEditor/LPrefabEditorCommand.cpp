﻿// Copyright 2019-Present LexLiu. All Rights Reserved.

#include "LPrefabEditorCommand.h"

#define LOCTEXT_NAMESPACE "LPrefabEditorCommand"

void FLPrefabEditorCommand::RegisterCommands()
{
	UI_COMMAND(Apply, "Apply", "Apply changes to prefab.", EUserInterfaceActionType::Button, FInputChord());
	UI_COMMAND(RawDataViewer, "RawDataViewer", "Open raw data viewer panel of this prefab.", EUserInterfaceActionType::Button, FInputChord());
	UI_COMMAND(OpenPrefabHelperObject, "PrefabHelperObject", "Open PrefabHelperObject details panel of this prefab.", EUserInterfaceActionType::Button, FInputChord());
	UI_COMMAND(CopyActor, "Copy Actors", "Copy selected actors with hierarchy", EUserInterfaceActionType::Button, FInputChord(EKeys::C, EModifierKey::Shift | EModifierKey::Alt));
	UI_COMMAND(PasteActor, "Paste Actors", "Paste actors with hierarchy", EUserInterfaceActionType::Button, FInputChord(EKeys::V, EModifierKey::Shift | EModifierKey::Alt));
	UI_COMMAND(CutActor, "Cut Actors", "Cut actors with hierarchy", EUserInterfaceActionType::Button, FInputChord(EKeys::X, EModifierKey::Shift | EModifierKey::Alt));
	UI_COMMAND(DuplicateActor, "Duplicate Actors", "Duplicate selected actors with hierarchy", EUserInterfaceActionType::Button, FInputChord(EKeys::D, EModifierKey::Shift | EModifierKey::Alt));
	UI_COMMAND(DestroyActor, "Destroy Actors", "Destroy selected actors with hierarchy", EUserInterfaceActionType::Button, FInputChord(EKeys::Delete));
}

#undef LOCTEXT_NAMESPACE