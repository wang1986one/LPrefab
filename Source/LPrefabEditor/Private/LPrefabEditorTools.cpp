﻿// Copyright 2019-Present LexLiu. All Rights Reserved.

#include "LPrefabEditorTools.h"
#include "Widgets/Docking/SDockTab.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "PropertyCustomizationHelpers.h"
#include "DesktopPlatformModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/EngineTypes.h"
#include "Kismet2/ComponentEditorUtils.h"
#include "Widgets/SViewport.h"
#include "EditorViewportClient.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "DataFactory/LPrefabActorFactory.h"
#include "PrefabSystem/LPrefabHelperObject.h"
#include LPREFAB_SERIALIZER_NEWEST_INCLUDE
#include "LPrefabEditorModule.h"
#include "PrefabEditor/LPrefabEditor.h"
#include "LPrefabHeaders.h"
#include "Logging/MessageLog.h"

#include "Settings/LevelEditorMiscSettings.h"
#include "Layers/LayersSubsystem.h"
#include "ActorEditorUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Serialization/ArchiveReplaceObjectRef.h"

#define LOCTEXT_NAMESPACE "LPrefabEditorTools"

PRAGMA_DISABLE_OPTIMIZATION

FEditingPrefabChangedDelegate LPrefabEditorTools::OnEditingPrefabChanged;
FBeforeApplyPrefabDelegate LPrefabEditorTools::OnBeforeApplyPrefab;

namespace ReattachActorsHelper
{
	/** Holds the actor and socket name for attaching. */
	struct FActorAttachmentInfo
	{
		AActor* Actor;

		FName SocketName;
	};

	/** Used to cache the attachment info for an actor. */
	struct FActorAttachmentCache
	{
	public:
		/** The post-conversion actor. */
		AActor* NewActor;

		/** The parent actor and socket. */
		FActorAttachmentInfo ParentActor;

		/** Children actors and the sockets they were attached to. */
		TArray<FActorAttachmentInfo> AttachedActors;
	};

	/**
	 * Caches the attachment info for the actors being converted.
	 *
	 * @param InActorsToReattach			List of actors to reattach.
	 * @param InOutAttachmentInfo			List of attachment info for the list of actors.
	 */
	void CacheAttachments(const TArray<AActor*>& InActorsToReattach, TArray<FActorAttachmentCache>& InOutAttachmentInfo)
	{
		for (int32 ActorIdx = 0; ActorIdx < InActorsToReattach.Num(); ++ActorIdx)
		{
			AActor* ActorToReattach = InActorsToReattach[ActorIdx];

			InOutAttachmentInfo.AddZeroed();

			FActorAttachmentCache& CurrentAttachmentInfo = InOutAttachmentInfo[ActorIdx];

			// Retrieve the list of attached actors.
			TArray<AActor*> AttachedActors;
			ActorToReattach->GetAttachedActors(AttachedActors);

			// Cache the parent actor and socket name.
			CurrentAttachmentInfo.ParentActor.Actor = ActorToReattach->GetAttachParentActor();
			CurrentAttachmentInfo.ParentActor.SocketName = ActorToReattach->GetAttachParentSocketName();

			// Required to restore attachments properly.
			for (int32 AttachedActorIdx = 0; AttachedActorIdx < AttachedActors.Num(); ++AttachedActorIdx)
			{
				// Store the attached actor and socket name in the cache.
				CurrentAttachmentInfo.AttachedActors.AddZeroed();
				CurrentAttachmentInfo.AttachedActors[AttachedActorIdx].Actor = AttachedActors[AttachedActorIdx];
				CurrentAttachmentInfo.AttachedActors[AttachedActorIdx].SocketName = AttachedActors[AttachedActorIdx]->GetAttachParentSocketName();

				AActor* ChildActor = CurrentAttachmentInfo.AttachedActors[AttachedActorIdx].Actor;
				ChildActor->Modify();
				ChildActor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
			}

			// Modify the actor so undo will reattach it.
			ActorToReattach->Modify();
			ActorToReattach->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		}
	}

	/**
	 * Caches the actor old/new information, mapping the old actor to the new version for easy look-up and matching.
	 *
	 * @param InOldActor			The old version of the actor.
	 * @param InNewActor			The new version of the actor.
	 * @param InOutReattachmentMap	Map object for placing these in.
	 * @param InOutAttachmentInfo	Update the required attachment info to hold the Converted Actor.
	 */
	void CacheActorConvert(AActor* InOldActor, AActor* InNewActor, TMap<AActor*, AActor*>& InOutReattachmentMap, FActorAttachmentCache& InOutAttachmentInfo)
	{
		// Add mapping data for the old actor to the new actor.
		InOutReattachmentMap.Add(InOldActor, InNewActor);

		// Set the converted actor so re-attachment can occur.
		InOutAttachmentInfo.NewActor = InNewActor;
	}

	/**
	 * Checks if two actors can be attached, creates Message Log messages if there are issues.
	 *
	 * @param InParentActor			The parent actor.
	 * @param InChildActor			The child actor.
	 * @param InOutErrorMessages	Errors with attaching the two actors are stored in this array.
	 *
	 * @return Returns true if the actors can be attached, false if they cannot.
	 */
	bool CanParentActors(AActor* InParentActor, AActor* InChildActor)
	{
		FText ReasonText;
		if (GEditor->CanParentActors(InParentActor, InChildActor, &ReasonText))
		{
			return true;
		}
		else
		{
			FMessageLog("EditorErrors").Error(ReasonText);
			return false;
		}
	}

	/**
	 * Reattaches actors to maintain the hierarchy they had previously using a conversion map and an array of attachment info. All errors displayed in Message Log along with notifications.
	 *
	 * @param InReattachmentMap			Used to find the corresponding new versions of actors using an old actor pointer.
	 * @param InAttachmentInfo			Holds parent and child attachment data.
	 */
	void ReattachActors(TMap<AActor*, AActor*>& InReattachmentMap, TArray<FActorAttachmentCache>& InAttachmentInfo)
	{
		// Holds the errors for the message log.
		FMessageLog EditorErrors("EditorErrors");
		EditorErrors.NewPage(LOCTEXT("AttachmentLogPage", "Actor Reattachment"));

		for (int32 ActorIdx = 0; ActorIdx < InAttachmentInfo.Num(); ++ActorIdx)
		{
			FActorAttachmentCache& CurrentAttachment = InAttachmentInfo[ActorIdx];

			// Need to reattach all of the actors that were previously attached.
			for (int32 AttachedIdx = 0; AttachedIdx < CurrentAttachment.AttachedActors.Num(); ++AttachedIdx)
			{
				// Check if the attached actor was converted. If it was it will be in the TMap.
				AActor** CheckIfConverted = InReattachmentMap.Find(CurrentAttachment.AttachedActors[AttachedIdx].Actor);
				if (CheckIfConverted)
				{
					// This should always be valid.
					if (*CheckIfConverted)
					{
						AActor* ParentActor = CurrentAttachment.NewActor;
						AActor* ChildActor = *CheckIfConverted;

						if (CanParentActors(ParentActor, ChildActor))
						{
							// Attach the previously attached and newly converted actor to the current converted actor.
							ChildActor->AttachToActor(ParentActor, FAttachmentTransformRules::KeepWorldTransform, CurrentAttachment.AttachedActors[AttachedIdx].SocketName);
						}
					}

				}
				else
				{
					AActor* ParentActor = CurrentAttachment.NewActor;
					AActor* ChildActor = CurrentAttachment.AttachedActors[AttachedIdx].Actor;

					if (CanParentActors(ParentActor, ChildActor))
					{
						// Since the actor was not converted, reattach the unconverted actor.
						ChildActor->AttachToActor(ParentActor, FAttachmentTransformRules::KeepWorldTransform, CurrentAttachment.AttachedActors[AttachedIdx].SocketName);
					}
				}

			}

			// Check if the parent was converted.
			AActor** CheckIfNewActor = InReattachmentMap.Find(CurrentAttachment.ParentActor.Actor);
			if (CheckIfNewActor)
			{
				// Since the actor was converted, attach the current actor to it.
				if (*CheckIfNewActor)
				{
					AActor* ParentActor = *CheckIfNewActor;
					AActor* ChildActor = CurrentAttachment.NewActor;

					if (CanParentActors(ParentActor, ChildActor))
					{
						ChildActor->AttachToActor(ParentActor, FAttachmentTransformRules::KeepWorldTransform, CurrentAttachment.ParentActor.SocketName);
					}
				}

			}
			else
			{
				AActor* ParentActor = CurrentAttachment.ParentActor.Actor;
				AActor* ChildActor = CurrentAttachment.NewActor;

				// Verify the parent is valid, the actor may not have actually been attached before.
				if (ParentActor && CanParentActors(ParentActor, ChildActor))
				{
					// The parent was not converted, attach to the unconverted parent.
					ChildActor->AttachToActor(ParentActor, FAttachmentTransformRules::KeepWorldTransform, CurrentAttachment.ParentActor.SocketName);
				}
			}
		}

		// Add the errors to the message log, notifications will also be displayed as needed.
		EditorErrors.Notify(NSLOCTEXT("ActorAttachmentError", "AttachmentsFailed", "Attachments Failed!"));
	}
}

struct LPrefabEditorToolsHelperFunctionHolder
{
public:
	static TArray<AActor*> ConvertSelectionToActors(USelection* InSelection)
	{
		TArray<AActor*> result;
		auto count = InSelection->Num();
		for (int i = 0; i < count; i++)
		{
			auto obj = (AActor*)(InSelection->GetSelectedObject(i));
			if (obj != nullptr)
			{
				result.Add(obj);
			}
		}
		return result;
	}
	static FString GetLabelPrefixForCopy(const FString& srcActorLabel, FString& outNumetricSuffix)
	{
		int rightCount = 1;
		while (rightCount <= srcActorLabel.Len() && srcActorLabel.Right(rightCount).IsNumeric())
		{
			rightCount++;
		}
		rightCount--;
		outNumetricSuffix = srcActorLabel.Right(rightCount);
		return srcActorLabel.Left(srcActorLabel.Len() - rightCount);
	}
public:
	static FString GetCopiedActorLabel(AActor* Parent, FString OriginActorLabel, UWorld* World)
	{
		TArray<AActor*> SameParentActorList;//all actors attached at same parent actor. if parent is null then get all actors
		for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
		{
			if (AActor* itemActor = *ActorItr)
			{
				if (IsValid(itemActor))
				{
					if (IsValid(Parent))
					{
						if (itemActor->GetAttachParentActor() == Parent)
						{
							SameParentActorList.Add(itemActor);
						}
					}
					else
					{
						if (itemActor->GetAttachParentActor() == nullptr)
						{
							SameParentActorList.Add(itemActor);
						}
					}
				}
			}
		}
	

		FString MaxNumetricSuffixStr = TEXT("");//numetric suffix
		OriginActorLabel = GetLabelPrefixForCopy(OriginActorLabel, MaxNumetricSuffixStr);
		int MaxNumetricSuffixStrLength = MaxNumetricSuffixStr.Len();
		int SameNameActorCount = 0;//if actor name is same with source name, then collect it
		for (int i = 0; i < SameParentActorList.Num(); i ++)//search from same level actors, and get the right suffix
		{
			auto item = SameParentActorList[i];
			auto itemActorLabel = item->GetActorLabel();
			if (itemActorLabel == OriginActorLabel)SameNameActorCount++;
			if (OriginActorLabel.Len() == 0 || itemActorLabel.StartsWith(OriginActorLabel))
			{
				auto itemRightStr = itemActorLabel.Right(itemActorLabel.Len() - OriginActorLabel.Len());
				if (!itemRightStr.IsNumeric())//if rest is not numetric
				{
					continue;
				}
				FString itemNumetrixSuffixStr = itemRightStr;
				int itemNumetrix = FCString::Atoi(*itemNumetrixSuffixStr);
				int maxNumetrixSuffix = FCString::Atoi(*MaxNumetricSuffixStr);
				if (itemNumetrix > maxNumetrixSuffix)
				{
					maxNumetrixSuffix = itemNumetrix;
					MaxNumetricSuffixStr = FString::Printf(TEXT("%d"), maxNumetrixSuffix);
				}
			}
		}
		FString CopiedActorLabel = OriginActorLabel;
		if (!MaxNumetricSuffixStr.IsEmpty() || SameNameActorCount > 0)
		{
			int MaxNumtrixSuffix = FCString::Atoi(*MaxNumetricSuffixStr);
			MaxNumtrixSuffix++;
			FString NumetrixSuffixStr = FString::Printf(TEXT("%d"), MaxNumtrixSuffix);
			while (NumetrixSuffixStr.Len() < MaxNumetricSuffixStrLength)
			{
				NumetrixSuffixStr = TEXT("0") + NumetrixSuffixStr;
			}
			CopiedActorLabel += NumetrixSuffixStr;
		}
		return CopiedActorLabel;
	}
	
public:
	static TArray<UActorComponent*> ConvertSelectionToComponents(USelection* InSelection)
	{
		TArray<UActorComponent*> result;
		auto count = InSelection->Num();
		for (int i = 0; i < count; i++)
		{
			auto obj = (UActorComponent*)(InSelection->GetSelectedObject(i));
			if (obj != nullptr)
			{
				result.Add(obj);
			}
		}
		return result;
	}

	//this function mostly copied from UnrealED/Private/EditorEngine.cpp::ReplaceActors
	static TArray<AActor*> ReplaceActor(const TArray<AActor*>& ActorsToReplace, TSubclassOf<AActor> NewActorClass)
	{
		TArray<AActor*> Result;
		// Cache for attachment info of all actors being converted.
		TArray<ReattachActorsHelper::FActorAttachmentCache> AttachmentInfo;

		// Maps actors from old to new for quick look-up.
		TMap<AActor*, AActor*> ConvertedMap;

		// Cache the current attachment states.
		ReattachActorsHelper::CacheAttachments(ActorsToReplace, AttachmentInfo);

		USelection* SelectedActors = GEditor->GetSelectedActors();
		SelectedActors->BeginBatchSelectOperation();
		SelectedActors->Modify();

		for (int32 ActorIdx = 0; ActorIdx < ActorsToReplace.Num(); ++ActorIdx)
		{
			AActor* OldActor = ActorsToReplace[ActorIdx];//.Pop();
			check(OldActor);
			UWorld* World = OldActor->GetWorld();
			ULevel* Level = OldActor->GetLevel();
			AActor* NewActor = NULL;

			// Unregister this actors components because we are effectively replacing it with an actor sharing the same ActorGuid.
			// This allows it to be unregistered before a new actor with the same guid gets registered avoiding conflicts.
			OldActor->UnregisterAllComponents();

			const FName OldActorName = OldActor->GetFName();
			const FName OldActorReplacedNamed = MakeUniqueObjectName(OldActor->GetOuter(), OldActor->GetClass(), *FString::Printf(TEXT("%s_REPLACED"), *OldActorName.ToString()));

			FActorSpawnParameters SpawnParams;
			SpawnParams.Name = OldActorName;
			SpawnParams.bCreateActorPackage = false;
			SpawnParams.OverridePackage = OldActor->GetExternalPackage();
			SpawnParams.OverrideActorGuid = OldActor->GetActorGuid();

			// Don't go through AActor::Rename here because we aren't changing outers (the actor's level) and we also don't want to reset loaders
			// if the actor is using an external package. We really just want to rename that actor out of the way so we can spawn the new one in
			// the exact same package, keeping the package name intact.
			OldActor->UObject::Rename(*OldActorReplacedNamed.ToString(), OldActor->GetOuter(), REN_DoNotDirty | REN_DontCreateRedirectors | REN_ForceNoResetLoaders);

			const FTransform OldTransform = OldActor->ActorToWorld();

			// create the actor
			NewActor = OldActor->GetWorld()->SpawnActor(NewActorClass, &OldTransform, SpawnParams);
			//added by liuf, if no root component then add one
			{
				auto RootComponent = NewActor->GetRootComponent();
				if (!RootComponent)
				{
					RootComponent = NewObject<USceneComponent>(NewActor, USceneComponent::GetDefaultSceneRootVariableName(), RF_Transactional);
					RootComponent->Mobility = EComponentMobility::Movable;
					RootComponent->bVisualizeComponent = false;

					NewActor->SetRootComponent(RootComponent);
					RootComponent->RegisterComponent();
					NewActor->AddInstanceComponent(RootComponent);
				}
			}
			// try to copy over properties
			NewActor->UnregisterAllComponents();
			UEngine::FCopyPropertiesForUnrelatedObjectsParams Options;
			Options.bNotifyObjectReplacement = true;
			UEditorEngine::CopyPropertiesForUnrelatedObjects(OldActor, NewActor, Options);
			if (OldActor->GetRootComponent() != nullptr && NewActor->GetRootComponent() != nullptr)
			{
				UEditorEngine::CopyPropertiesForUnrelatedObjects(OldActor->GetRootComponent(), NewActor->GetRootComponent(), Options);
			}
			NewActor->RegisterAllComponents();
			Result.Add(NewActor);

			if (NewActor)
			{
				// The new actor might not have a root component
				USceneComponent* const NewActorRootComponent = NewActor->GetRootComponent();
				if (NewActorRootComponent)
				{
					if (!GetDefault<ULevelEditorMiscSettings>()->bReplaceRespectsScale || OldActor->GetRootComponent() == NULL)
					{
						NewActorRootComponent->SetRelativeScale3D(FVector(1.0f, 1.0f, 1.0f));
					}
					else
					{
						NewActorRootComponent->SetRelativeScale3D(OldActor->GetRootComponent()->GetRelativeScale3D());
					}

					if (OldActor->GetRootComponent() != NULL)
					{
						NewActorRootComponent->SetMobility(OldActor->GetRootComponent()->Mobility);
					}
				}

				NewActor->Layers.Empty();
				ULayersSubsystem* LayersSubsystem = GEditor->GetEditorSubsystem<ULayersSubsystem>();
				LayersSubsystem->AddActorToLayers(NewActor, OldActor->Layers);

				// Preserve the label and tags from the old actor
				NewActor->SetActorLabel(OldActor->GetActorLabel());
				NewActor->Tags = OldActor->Tags;

				// Allow actor derived classes a chance to replace properties.
				NewActor->EditorReplacedActor(OldActor);

				// Caches information for finding the new actor using the pre-converted actor.
				ReattachActorsHelper::CacheActorConvert(OldActor, NewActor, ConvertedMap, AttachmentInfo[ActorIdx]);

				if (SelectedActors->IsSelected(OldActor))
				{
					GEditor->SelectActor(OldActor, false, true);
					GEditor->SelectActor(NewActor, true, true);
				}

				// Find compatible static mesh components and copy instance colors between them.
				UStaticMeshComponent* NewActorStaticMeshComponent = NewActor->FindComponentByClass<UStaticMeshComponent>();
				UStaticMeshComponent* OldActorStaticMeshComponent = OldActor->FindComponentByClass<UStaticMeshComponent>();
				if (NewActorStaticMeshComponent != NULL && OldActorStaticMeshComponent != NULL)
				{
					NewActorStaticMeshComponent->CopyInstanceVertexColorsIfCompatible(OldActorStaticMeshComponent);
				}

				NewActor->InvalidateLightingCache();
				NewActor->PostEditMove(true);
				NewActor->MarkPackageDirty();

				TSet<ULevel*> LevelsToRebuildBSP;
				ABrush* Brush = Cast<ABrush>(OldActor);
				if (Brush && !FActorEditorUtils::IsABuilderBrush(Brush)) // Track whether or not a brush actor was deleted.
				{
					ULevel* BrushLevel = OldActor->GetLevel();
					if (BrushLevel && !Brush->IsVolumeBrush())
					{
						BrushLevel->Model->Modify();
						LevelsToRebuildBSP.Add(BrushLevel);
					}
				}

				// Replace references in the level script Blueprint with the new Actor
				const bool bDontCreate = true;
				ULevelScriptBlueprint* LSB = NewActor->GetLevel()->GetLevelScriptBlueprint(bDontCreate);
				if (LSB)
				{
					// Only if the level script blueprint exists would there be references.  
					FBlueprintEditorUtils::ReplaceAllActorRefrences(LSB, OldActor, NewActor);
				}

				LayersSubsystem->DisassociateActorFromLayers(OldActor);
				World->EditorDestroyActor(OldActor, true);

				// If any brush actors were modified, update the BSP in the appropriate levels
				if (LevelsToRebuildBSP.Num())
				{
					FlushRenderingCommands();

					for (ULevel* LevelToRebuild : LevelsToRebuildBSP)
					{
						GEditor->RebuildLevel(*LevelToRebuild);
					}
				}
			}
			else
			{
				// If creating the new Actor failed, put the old Actor's name back
				OldActor->UObject::Rename(*OldActorName.ToString(), OldActor->GetOuter(), REN_DoNotDirty | REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
				OldActor->RegisterAllComponents();
			}
		}

		SelectedActors->EndBatchSelectOperation();

		// Reattaches actors based on their previous parent child relationship.
		ReattachActorsHelper::ReattachActors(ConvertedMap, AttachmentInfo);

		// Perform reference replacement on all Actors referenced by World
		TArray<UObject*> ReferencedLevels;

		for (const TPair<AActor*, AActor*>& ReplacedObj : ConvertedMap)
		{
			ReferencedLevels.AddUnique(ReplacedObj.Value->GetLevel());
		}

		for (UObject* Referencer : ReferencedLevels)
		{
			constexpr EArchiveReplaceObjectFlags ArFlags = (EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::TrackReplacedReferences);
			FArchiveReplaceObjectRef<AActor> Ar(Referencer, ConvertedMap, ArFlags);

			for (const TPair<UObject*, TArray<FProperty*>>& MapItem : Ar.GetReplacedReferences())
			{
				UObject* ModifiedObject = MapItem.Key;

				if (!ModifiedObject->HasAnyFlags(RF_Transient) && ModifiedObject->GetOutermost() != GetTransientPackage() && !ModifiedObject->RootPackageHasAnyFlags(PKG_CompiledIn))
				{
					ModifiedObject->MarkPackageDirty();
				}

				for (FProperty* Property : MapItem.Value)
				{
					FPropertyChangedEvent PropertyEvent(Property);
					ModifiedObject->PostEditChangeProperty(PropertyEvent);
				}
			}
		}

		GEditor->RedrawLevelEditingViewports();

		ULevel::LevelDirtiedEvent.Broadcast();

		return Result;
	}
};

TMap<FString, TWeakObjectPtr<class ULPrefab>> LPrefabEditorTools::CopiedActorPrefabMap;
TWeakObjectPtr<class UActorComponent> LPrefabEditorTools::CopiedComponent;

FString LPrefabEditorTools::GetUniqueNumetricName(const FString& InPrefix, const TArray<FString>& InExistNames)
{
	auto ExtractNumetric = [](const FString& InString, int32& Num) {
		int NumetricStringIndex = -1;
		FString SubNumetricString;
		int NumetricStringCharCount = 0;
		for (int i = InString.Len() - 1; i >= 0; i--)
		{
			auto SubChar = InString[i];
			if (SubChar >= '0' && SubChar <= '9')
			{
				NumetricStringIndex = i;

				NumetricStringCharCount++;
				if (NumetricStringCharCount >= 4)
				{
					break;
				}
			}
			else
			{
				break;
			}
		}
		if (NumetricStringIndex != -1)
		{
			auto NumetricSubString = InString.Right(InString.Len() - NumetricStringIndex);
			Num = FCString::Atoi(*NumetricSubString);
			return true;
		}
		else
		{
			return false;
		}
	};
	int MaxNumSuffix = 0;
	for (int i = 0; i < InExistNames.Num(); i++)//search from same level actors, and get the right suffix
	{
		auto& Item = InExistNames[i];
		if (Item.Len() == 0)continue;
		int Num;
		if (ExtractNumetric(Item, Num))
		{
			if (Num > MaxNumSuffix)
			{
				MaxNumSuffix = Num;
			}
		}
	}
	return FString::Printf(TEXT("%s_%d"), *InPrefix, MaxNumSuffix + 1);
}

TArray<AActor*> LPrefabEditorTools::GetRootActorListFromSelection(const TArray<AActor*>& selectedActors)
{
	TArray<AActor*> RootActorList;
	auto count = selectedActors.Num();
	//search upward find parent and put into list, only root actor can add to list
	for (int i = 0; i < count; i++)
	{
		auto obj = selectedActors[i];
		auto parent = obj->GetAttachParentActor();
		bool isRootActor = false;
		while (true)
		{
			if (parent == nullptr)//top level
			{
				isRootActor = true;
				break;
			}
			if (selectedActors.Contains(parent))//if parent is already in list, skip it
			{
				isRootActor = false;
				break;
			}
			else//if not in list, keep search upward
			{
				parent = parent->GetAttachParentActor();
				continue;
			}
		}
		if (isRootActor)
		{
			RootActorList.Add(obj);
		}
	}
	return RootActorList;
}
UWorld* LPrefabEditorTools::GetWorldFromSelection()
{
	if (auto selectedActor = GetFirstSelectedActor())
	{
		return selectedActor->GetWorld();
	}
	return GWorld;
}
void LPrefabEditorTools::CreateEmptyActor()
{
	auto selectedActor = GetFirstSelectedActor();
	if (selectedActor == nullptr)return;
	GEditor->BeginTransaction(LOCTEXT("CreateEmptyActor_Transaction", "LGUI create empty actor"));
	MakeCurrentLevel(selectedActor);
	AActor* newActor = GetWorldFromSelection()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, FActorSpawnParameters());
	if (IsValid(newActor))
	{
		//create SceneComponent
		{
			USceneComponent* RootComponent = NewObject<USceneComponent>(newActor, USceneComponent::GetDefaultSceneRootVariableName(), RF_Transactional);
			RootComponent->Mobility = EComponentMobility::Movable;
			RootComponent->bVisualizeComponent = false;

			newActor->SetRootComponent(RootComponent);
			RootComponent->RegisterComponent();
			newActor->AddInstanceComponent(RootComponent);
		}
		if (selectedActor != nullptr)
		{
			newActor->AttachToActor(selectedActor, FAttachmentTransformRules::KeepRelativeTransform);
			GEditor->SelectActor(selectedActor, false, true);
		}
		GEditor->SelectActor(newActor, true, true);
	}
	GEditor->EndTransaction();
}

AActor* LPrefabEditorTools::GetFirstSelectedActor()
{
	auto selectedActors = GetSelectedActors();
	auto count = selectedActors.Num();
	if (count == 0)
	{
		//UE_LOG(LPrefabEditor, Error, TEXT("NothingSelected"));
		return nullptr;
	}
	else if (count > 1)
	{
		//UE_LOG(LPrefabEditor, Error, TEXT("Only support one component"));
		return nullptr;
	}
	return selectedActors[0];
}

TArray<AActor*> LPrefabEditorTools::GetSelectedActors()
{
	return LPrefabEditorToolsHelperFunctionHolder::ConvertSelectionToActors(GEditor->GetSelectedActors());
}

void LPrefabEditorTools::ReplaceActorByClass(UClass* ActorClass)
{
	auto selectedActors = GetSelectedActors();
	auto count = selectedActors.Num();
	if (count == 0)
	{
		UE_LOG(LPrefabEditor, Error, TEXT("NothingSelected"));
		return;
	}
	auto RootActorList = LPrefabEditorTools::GetRootActorListFromSelection(selectedActors);

	GEditor->BeginTransaction(LOCTEXT("ReplaceUIElement_Transaction", "LGUI Replace UI Element"));
	for (auto& Actor : RootActorList)
	{
		MakeCurrentLevel(Actor);
		TFunction<void(AActor*)> OnEndFunction = nullptr;
		FLPrefabEditorModule::PrefabEditor_ReplaceActorByClass.ExecuteIfBound(Actor, OnEndFunction);

		AActor* ReplacedActor = nullptr;
		if (auto PrefabHelperObject = LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(Actor))
		{
			if (PrefabHelperObject->CleanupInvalidSubPrefab())//do cleanup before everything else
			{
				PrefabHelperObject->Modify();
			}
			bool bIsRootActor = PrefabHelperObject->LoadedRootActor == Actor;
			if (bIsRootActor)
			{
				auto confirmMsg = LOCTEXT("Warning_ReplaceRootActorOfPrefab", "Trying to replace root actor of a prefab, this could cause unexpected error if other prefab or level is referencing this prefab!\
\nDo you want to continue.");
				auto confirmResult = FMessageDialog::Open(EAppMsgType::YesNo, confirmMsg);
				if (confirmResult == EAppReturnType::Yes)
				{
					auto FindGuid = [&](UObject* Obj) {
						for (auto& KeyValue : PrefabHelperObject->MapGuidToObject)
						{
							if (Obj == KeyValue.Value)
							{
								return KeyValue.Key;
							}
						}
						return FGuid();
					};
					TArray<UObject*> OriginObjects;
					GetObjectsWithOuter(PrefabHelperObject->LoadedRootActor, OriginObjects);
					TMap<FName, FGuid> MapObjectNameToGuid;
					for (auto& Object : OriginObjects)
					{
						auto FoundGuid = FindGuid(Object);
						if (FoundGuid.IsValid())
						{
							MapObjectNameToGuid.Add(Object->GetFName(), FoundGuid);
						}
					}
					FGuid RootActorGuid = FindGuid(PrefabHelperObject->LoadedRootActor);
					FGuid RootCompGuid = FindGuid(PrefabHelperObject->LoadedRootActor->GetRootComponent());

					PrefabHelperObject->SetCanNotifyAttachment(false);
					ReplacedActor = LPrefabEditorToolsHelperFunctionHolder::ReplaceActor({ Actor }, ActorClass)[0];
					if (bIsRootActor)
					{
						PrefabHelperObject->LoadedRootActor = ReplacedActor;
					}
					TArray<UObject*> NewObjects;
					GetObjectsWithOuter(ReplacedActor, NewObjects);
					for (auto& KeyValue : MapObjectNameToGuid)
					{
						auto FoundIndex = NewObjects.IndexOfByPredicate([=](const UObject* Item) {
							return Item->GetFName() == KeyValue.Key;
							});
						if (FoundIndex != INDEX_NONE)
						{
							PrefabHelperObject->MapGuidToObject[KeyValue.Value] = NewObjects[FoundIndex];
						}
					}
					PrefabHelperObject->MapGuidToObject[RootActorGuid] = ReplacedActor;
					PrefabHelperObject->MapGuidToObject[RootCompGuid] = ReplacedActor->GetRootComponent();

					PrefabHelperObject->SetCanNotifyAttachment(true);
				}
			}
			else
			{
				ReplacedActor = LPrefabEditorToolsHelperFunctionHolder::ReplaceActor({ Actor }, ActorClass)[0];
			}
			PrefabHelperObject->SetAnythingDirty();
		}
		else
		{
			ReplacedActor = LPrefabEditorToolsHelperFunctionHolder::ReplaceActor({ Actor }, ActorClass)[0];
		}
		if (IsValid(ReplacedActor))
		{
			if (OnEndFunction != nullptr)
			{
				OnEndFunction(ReplacedActor);
			}
		}
	}
	GEditor->EndTransaction();
}
void LPrefabEditorTools::DuplicateSelectedActors_Impl()//@todo: fix bug: duplicate subprefab then undo, this operation will revert the source copied prefab to orignal state
{
	auto selectedActors = GetSelectedActors();
	auto count = selectedActors.Num();
	if (count == 0)
	{
		UE_LOG(LPrefabEditor, Error, TEXT("NothingSelected"));
		return;
	}
	auto RootActorList = LPrefabEditorTools::GetRootActorListFromSelection(selectedActors);
	GEditor->BeginTransaction(LOCTEXT("DuplicateActor_Transaction", "LGUI Duplicate Actors"));
	for (auto Actor : RootActorList)
	{
		MakeCurrentLevel(Actor);
		Actor->GetLevel()->Modify();
		auto copiedActorLabel = LPrefabEditorToolsHelperFunctionHolder::GetCopiedActorLabel(Actor->GetAttachParentActor(), Actor->GetActorLabel(), Actor->GetWorld());
		AActor* copiedActor;
		USceneComponent* Parent = nullptr;
		if (Actor->GetAttachParentActor())
		{
			Parent = Actor->GetAttachParentActor()->GetRootComponent();
		}
		TMap<TObjectPtr<AActor>, FLSubPrefabData> DuplicatedSubPrefabMap;
		TMap<FGuid, TObjectPtr<UObject>> OutMapGuidToObject;
		TMap<UObject*, FGuid> InMapObjectToGuid;
		if (auto PrefabHelperObject = LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(Actor))
		{
			PrefabHelperObject->CleanupInvalidSubPrefab();//do cleanup before everything else
			PrefabHelperObject->Modify();
			PrefabHelperObject->SetCanNotifyAttachment(false);
			struct LOCAL {
				static void CollectSubPrefabActors(AActor* InActor, const TMap<TObjectPtr<AActor>, FLSubPrefabData>& InSubPrefabMap, TArray<AActor*>& OutSubPrefabRootActors)
				{
					if (InSubPrefabMap.Contains(InActor))
					{
						OutSubPrefabRootActors.Add(InActor);
					}
					else
					{
						TArray<AActor*> ChildrenActors;
						InActor->GetAttachedActors(ChildrenActors);
						for (auto& ChildActor : ChildrenActors)
						{
							CollectSubPrefabActors(ChildActor, InSubPrefabMap, OutSubPrefabRootActors);
						}
					}
				}
			};
			TArray<AActor*> SubPrefabRootActors;
			LOCAL::CollectSubPrefabActors(Actor, PrefabHelperObject->SubPrefabMap, SubPrefabRootActors);//collect sub prefabs that is attached to this Actor
			for (auto& SubPrefabKeyValue : PrefabHelperObject->SubPrefabMap)//generate MapObjectToGuid
			{
				auto SubPrefabRootActor = SubPrefabKeyValue.Key;
				if (SubPrefabRootActors.Contains(SubPrefabRootActor))
				{
					auto& SubPrefabData = SubPrefabKeyValue.Value;
					PrefabHelperObject->RefreshOnSubPrefabDirty(SubPrefabData.PrefabAsset, SubPrefabRootActor);//need to update subprefab to latest before duplicate
					auto FindObjectGuidInParentPrefab = [&](FGuid InGuidInSubPrefab) {
						for (auto& KeyValue : SubPrefabData.MapObjectGuidFromParentPrefabToSubPrefab)
						{
							if (KeyValue.Value == InGuidInSubPrefab)
							{
								return KeyValue.Key;
							}
						}
						UE_LOG(LPrefabEditor, Error, TEXT("[LPrefabEditorTools::DuplicateSelectedActors_Impl] Should never reach this point!"));
						FDebug::DumpStackTraceToLog(ELogVerbosity::Warning);
						return FGuid::NewGuid();
					};
					for (auto& MapGuidToObjectKeyValue : SubPrefabData.MapGuidToObject)
					{
						InMapObjectToGuid.Add(MapGuidToObjectKeyValue.Value, FindObjectGuidInParentPrefab(MapGuidToObjectKeyValue.Key));
					}
				}
			}
			copiedActor = LPREFAB_SERIALIZER_NEWEST_NAMESPACE::ActorSerializer::DuplicateActorForEditor(Actor, Parent, PrefabHelperObject->SubPrefabMap, InMapObjectToGuid, DuplicatedSubPrefabMap, OutMapGuidToObject);
			for (auto& KeyValue : DuplicatedSubPrefabMap)
			{
				TMap<FGuid, TObjectPtr<UObject>> SubMapGuidToObject;
				for (auto& MapGuidItem : KeyValue.Value.MapObjectGuidFromParentPrefabToSubPrefab)
				{
					SubMapGuidToObject.Add(MapGuidItem.Value, OutMapGuidToObject[MapGuidItem.Key]);
				}
				PrefabHelperObject->MakePrefabAsSubPrefab(KeyValue.Value.PrefabAsset, KeyValue.Key, SubMapGuidToObject, KeyValue.Value.ObjectOverrideParameterArray);
			}
			PrefabHelperObject->SetCanNotifyAttachment(true);
		}
		else 
		{
			copiedActor = LPREFAB_SERIALIZER_NEWEST_NAMESPACE::ActorSerializer::DuplicateActorForEditor(Actor, Parent, {}, InMapObjectToGuid, DuplicatedSubPrefabMap, OutMapGuidToObject);
		}
		copiedActor->SetActorLabel(copiedActorLabel);
		GEditor->SelectActor(Actor, false, true);
		GEditor->SelectActor(copiedActor, true, true);
	}
	GEditor->EndTransaction();
	FLPrefabEditorModule::PrefabEditor_EndDuplicateActors.ExecuteIfBound();
}
void LPrefabEditorTools::CopySelectedActors_Impl()
{
	auto selectedActors = GetSelectedActors();
	auto count = selectedActors.Num();
	if (count == 0)
	{
		UE_LOG(LPrefabEditor, Error, TEXT("NothingSelected"));
		return;
	}
	for (auto KeyValuePair : CopiedActorPrefabMap)
	{
		KeyValuePair.Value->RemoveFromRoot();
		KeyValuePair.Value->ConditionalBeginDestroy();
	}
	auto CopyActorList = LPrefabEditorTools::GetRootActorListFromSelection(selectedActors);
	CopiedActorPrefabMap.Reset();
	for (auto Actor : CopyActorList)
	{
		auto prefab = NewObject<ULPrefab>();
		prefab->AddToRoot();
		TMap<UObject*, FGuid> MapObjectToGuid;
		TMap<TObjectPtr<AActor>, FLSubPrefabData> SubPrefabMap;
		if (auto PrefabHelperObject = LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(Actor))
		{
			SubPrefabMap = PrefabHelperObject->SubPrefabMap;

			if (PrefabHelperObject->CleanupInvalidSubPrefab())//do cleanup before everything else
			{
				PrefabHelperObject->Modify();
			}
			struct LOCAL {
				static void CollectSubPrefabActors(AActor* InActor, const TMap<TObjectPtr<AActor>, FLSubPrefabData>& InSubPrefabMap, TArray<AActor*>& OutSubPrefabRootActors)
				{
					if (InSubPrefabMap.Contains(InActor))
					{
						OutSubPrefabRootActors.Add(InActor);
					}
					else
					{
						TArray<AActor*> ChildrenActors;
						InActor->GetAttachedActors(ChildrenActors);
						for (auto& ChildActor : ChildrenActors)
						{
							CollectSubPrefabActors(ChildActor, InSubPrefabMap, OutSubPrefabRootActors);
						}
					}
				}
			};
			TArray<AActor*> SubPrefabRootActors;
			LOCAL::CollectSubPrefabActors(Actor, PrefabHelperObject->SubPrefabMap, SubPrefabRootActors);//collect sub prefabs that is attached to this Actor
			for (auto& SubPrefabKeyValue : PrefabHelperObject->SubPrefabMap)//generate MapObjectToGuid
			{
				auto SubPrefabRootActor = SubPrefabKeyValue.Key;
				if (SubPrefabRootActors.Contains(SubPrefabRootActor))
				{
					auto& SubPrefabData = SubPrefabKeyValue.Value;
					PrefabHelperObject->RefreshOnSubPrefabDirty(SubPrefabData.PrefabAsset, SubPrefabRootActor);//need to update subprefab to latest before duplicate
					auto FindObjectGuidInParentPrefab = [&](FGuid InGuidInSubPrefab) {
						for (auto& KeyValue : SubPrefabData.MapObjectGuidFromParentPrefabToSubPrefab)
						{
							if (KeyValue.Value == InGuidInSubPrefab)
							{
								return KeyValue.Key;
							}
						}
						UE_LOG(LPrefabEditor, Error, TEXT("[LPrefabEditorTools::CopySelectedActors_Impl] Should never reach this point!"));
						FDebug::DumpStackTraceToLog(ELogVerbosity::Warning);
						return FGuid::NewGuid();
					};
					for (auto& MapGuidToObjectKeyValue : SubPrefabData.MapGuidToObject)
					{
						MapObjectToGuid.Add(MapGuidToObjectKeyValue.Value, FindObjectGuidInParentPrefab(MapGuidToObjectKeyValue.Key));
					}
				}
			}
		}

		TMap<TObjectPtr<AActor>, FLSubPrefabData> TempSubPrefabMap;
		for (auto& SubPrefabKeyValue : SubPrefabMap)
		{
			if (SubPrefabKeyValue.Key->IsAttachedTo(Actor) || SubPrefabKeyValue.Key == Actor)
			{
				TempSubPrefabMap = SubPrefabMap;
				break;
			}
		}
		prefab->SavePrefab(Actor, MapObjectToGuid, TempSubPrefabMap);
		CopiedActorPrefabMap.Add(Actor->GetActorLabel(), prefab);
	}
}
void LPrefabEditorTools::PasteSelectedActors_Impl()
{
	auto selectedActors = GetSelectedActors();
	USceneComponent* parentComp = nullptr;
	if (selectedActors.Num() > 0)
	{
		parentComp = selectedActors[0]->GetRootComponent();
	}
	ULPrefabHelperObject* PrefabHelperObject = nullptr;
	if (parentComp)
	{
		PrefabHelperObject = LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(parentComp->GetOwner());
	}
	if (!PrefabHelperObject)
	{
		UWorld* World = nullptr;
		if (parentComp != nullptr)
		{
			World = parentComp->GetWorld();
		}
		else
		{
			World = GWorld;
		}
		if (World)
		{
			if (auto Level = World->GetCurrentLevel())
			{
				if (auto ManagerActor = ALPrefabLevelManagerActor::GetInstance(Level))
				{
					PrefabHelperObject = ManagerActor->PrefabHelperObject;
				}
			}
		}
	}
	if (PrefabHelperObject == nullptr)return;

	PrefabHelperObject->SetCanNotifyAttachment(false);
	GEditor->BeginTransaction(LOCTEXT("PasteActor_Transaction", "LGUI Paste Actors"));
	for (auto item : selectedActors)
	{
		GEditor->SelectActor(item, false, true);
	}
	if (IsValid(parentComp))
	{
		MakeCurrentLevel(parentComp->GetOwner());
	}
	for (auto KeyValuePair : CopiedActorPrefabMap)
	{
		if (KeyValuePair.Value.IsValid())
		{
			TMap<FGuid, TObjectPtr<UObject>> OutMapGuidToObject;
			TMap<TObjectPtr<AActor>, FLSubPrefabData> LoadedSubPrefabMap;
			auto copiedActorLabel = LPrefabEditorToolsHelperFunctionHolder::GetCopiedActorLabel(parentComp->GetOwner(), KeyValuePair.Key, parentComp->GetWorld());
			auto copiedActor = KeyValuePair.Value->LoadPrefabInEditor(parentComp->GetWorld(), parentComp, LoadedSubPrefabMap, OutMapGuidToObject, false);
			for (auto& KeyValue : LoadedSubPrefabMap)
			{
				TMap<FGuid, TObjectPtr<UObject>> SubMapGuidToObject;
				for (auto& MapGuidItem : KeyValue.Value.MapObjectGuidFromParentPrefabToSubPrefab)
				{
					SubMapGuidToObject.Add(MapGuidItem.Value, OutMapGuidToObject[MapGuidItem.Key]);
				}
				PrefabHelperObject->MakePrefabAsSubPrefab(KeyValue.Value.PrefabAsset, KeyValue.Key, SubMapGuidToObject, KeyValue.Value.ObjectOverrideParameterArray);
			}
			copiedActor->SetActorLabel(copiedActorLabel);
			GEditor->SelectActor(copiedActor, true, true, true);
		}
		else
		{
			UE_LOG(LPrefabEditor, Error, TEXT("Source copied actor is missing!"));
		}
	}
	PrefabHelperObject->SetCanNotifyAttachment(true);
	GEditor->EndTransaction();
	FLPrefabEditorModule::PrefabEditor_EndPasteActors.ExecuteIfBound();
}
void LPrefabEditorTools::DeleteSelectedActors_Impl()
{
	auto selectedActors = GetSelectedActors();
	DeleteActors_Impl(selectedActors);
}
void LPrefabEditorTools::CutSelectedActors_Impl()
{
	CopySelectedActors_Impl();
	DeleteSelectedActors_Impl();
}
void LPrefabEditorTools::ToggleSelectedActorsSpatiallyLoaded_Impl()
{
	auto selectedActors = GetSelectedActors();
	auto count = selectedActors.Num();
	if (count == 0)
	{
		UE_LOG(LPrefabEditor, Error, TEXT("NothingSelected"));
		return;
	}
	struct LOCAL
	{
		static void SetSpatiallyLoadedValue_Recursive(AActor* Actor, bool value)
		{
			if (Actor->CanChangeIsSpatiallyLoadedFlag())
			{
				if (Actor->GetIsSpatiallyLoaded() != value)
				{
					Actor->SetIsSpatiallyLoaded(value);
					LPrefabUtils::NotifyPropertyChanged(Actor, AActor::GetIsSpatiallyLoadedPropertyName());
				}
			}
			TArray<AActor*> ChildActors;
			Actor->GetAttachedActors(ChildActors);
			for (auto ChildActor : ChildActors)
			{
				if (IsValid(ChildActor))
				{
					SetSpatiallyLoadedValue_Recursive(ChildActor, value);
				}
			}
		}
	};
	GEditor->BeginTransaction(LOCTEXT("ToggleSpatiallyLoaded_Transaction", "LGUI Toggle Actors IsSpatiallyLoaded"));
	auto ActorList = LPrefabEditorTools::GetRootActorListFromSelection(selectedActors);
	for (auto Actor : ActorList)
	{
		Actor->Modify();
		auto bIsSpatiallyLoadedValue = !Actor->GetIsSpatiallyLoaded();
		LOCAL::SetSpatiallyLoadedValue_Recursive(Actor, bIsSpatiallyLoadedValue);
	}
	GEditor->EndTransaction();
}
ECheckBoxState LPrefabEditorTools::GetActorSpatiallyLoadedProperty()
{
	auto selectedActors = GetSelectedActors();
	auto count = selectedActors.Num();
	if (count == 0)
	{
		return ECheckBoxState::Undetermined;
	}
	auto ActorList = LPrefabEditorTools::GetRootActorListFromSelection(selectedActors);
	bool bIsSpatiallyLoadedValue = ActorList[0]->GetIsSpatiallyLoaded();
	for (int i = 1; i < ActorList.Num(); i++)
	{
		if (bIsSpatiallyLoadedValue != ActorList[i]->GetIsSpatiallyLoaded())
		{
			return ECheckBoxState::Undetermined;
		}
	}
	return bIsSpatiallyLoadedValue ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void LPrefabEditorTools::DeleteActors_Impl(const TArray<AActor*>& InActors)
{
	auto count = InActors.Num();
	if (count == 0)
	{
		UE_LOG(LPrefabEditor, Error, TEXT("NothingSelected"));
		return;
	}
	auto confirmMsg = FString::Printf(TEXT("Destroy selected actors? This will also destroy the children attached to selected actors."));
	auto confirmResult = FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(confirmMsg));
	if (confirmResult == EAppReturnType::No)return;

	ULPrefabManagerObject::GetInstance(true)->bIsProcessingDelete = true;
	auto RootActorList = LPrefabEditorTools::GetRootActorListFromSelection(InActors);
	GEditor->BeginTransaction(LOCTEXT("DestroyActor_Transaction", "LGUI Destroy Actor"));
	GEditor->GetSelectedActors()->DeselectAll();
	for (auto Actor : RootActorList)
	{
		bool shouldDeletePrefab = false;

		auto PrefabHelperObject = LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(Actor);
		if (PrefabHelperObject != nullptr)
		{
			PrefabHelperObject->Modify();
			PrefabHelperObject->SetAnythingDirty();
			TArray<AActor*> ChildrenActors;
			LPrefabUtils::CollectChildrenActors(Actor, ChildrenActors);
			for (auto ChildActor : ChildrenActors)
			{
				PrefabHelperObject->RemoveSubPrefabByAnyActorOfSubPrefab(ChildActor);
			}
			LPrefabUtils::DestroyActorWithHierarchy(Actor);
		}
		else//common actor
		{
			LPrefabUtils::DestroyActorWithHierarchy(Actor);
		}
	}
	GEditor->EndTransaction();
	CleanupPrefabsInWorld(RootActorList[0]->GetWorld());
	ULPrefabManagerObject::GetInstance(true)->bIsProcessingDelete = false;
}

bool LPrefabEditorTools::CanDuplicateActor()
{
	auto SelectedActor = LPrefabEditorTools::GetFirstSelectedActor();
	if (SelectedActor == nullptr)return false;
	if (!LPrefabEditorTools::IsActorCompatibleWithLGUIToolsMenu(SelectedActor))return false;
	return true;
}
bool LPrefabEditorTools::CanCopyActor()
{
	auto SelectedActors = LPrefabEditorTools::GetSelectedActors();
	if (SelectedActors.Num() <= 0)return false;
	return true;
}
bool LPrefabEditorTools::CanPasteActor()
{
	if (LPrefabEditorTools::CopiedActorPrefabMap.Num() == 0)return false;
	auto SelectedActor = LPrefabEditorTools::GetFirstSelectedActor();
	if (SelectedActor == nullptr)return false;
	if (!LPrefabEditorTools::IsActorCompatibleWithLGUIToolsMenu(SelectedActor))return false;
	return true;
}
bool LPrefabEditorTools::CanCutActor()
{
	return CanDeleteActor();
}
bool LPrefabEditorTools::CanDeleteActor()
{
	auto SelectedActors = LPrefabEditorTools::GetSelectedActors();
	if (SelectedActors.Num() == 0)return false;
	for (auto Actor : SelectedActors)
	{
		if (auto PrefabHelperObject = LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(Actor))
		{
			if (!PrefabHelperObject->ActorIsSubPrefabRootActor(Actor)//allowed to delete sub prefab's root actor
				&& PrefabHelperObject->IsActorBelongsToSubPrefab(Actor))//not allowed to delete sub prefab's actor
			{
				return false;
			}
		}
	}
	return true;
}
bool LPrefabEditorTools::CanToggleActorSpatiallyLoaded()
{
	return GEditor->GetSelectedActorCount() > 0;
	auto selectedActors = GetSelectedActors();
	if (selectedActors.Num() <= 0)return false;
	auto ActorList = LPrefabEditorTools::GetRootActorListFromSelection(selectedActors);
	for (auto Actor : ActorList)
	{
		if (!Actor->CanChangeIsSpatiallyLoadedFlag())
		{
			return false;
		}
	}
	return true;
}

void LPrefabEditorTools::CopyComponentValues_Impl()
{
	auto selectedComponents = LPrefabEditorToolsHelperFunctionHolder::ConvertSelectionToComponents(GEditor->GetSelectedComponents());
	auto count = selectedComponents.Num();
	if (count == 0)
	{
		UE_LOG(LPrefabEditor, Error, TEXT("NothingSelected"));
		return;
	}
	else if (count > 1)
	{
		UE_LOG(LPrefabEditor, Error, TEXT("Only support one component"));
		return;
	}
	CopiedComponent = selectedComponents[0];
}
void LPrefabEditorTools::PasteComponentValues_Impl()
{
	auto selectedComponents = LPrefabEditorToolsHelperFunctionHolder::ConvertSelectionToComponents(GEditor->GetSelectedComponents());
	auto count = selectedComponents.Num();
	if (count == 0)
	{
		UE_LOG(LPrefabEditor, Error, TEXT("NothingSelected"));
		return;
	}
	if (CopiedComponent.IsValid())
	{
		GEditor->BeginTransaction(LOCTEXT("PasteComponentValues_Transaction", "LGUI Paste Component Proeprties"));
		UEngine::FCopyPropertiesForUnrelatedObjectsParams Options;
		Options.bNotifyObjectReplacement = true;
		for (UActorComponent* SelectedComp : selectedComponents)
		{
			if (SelectedComp->IsRegistered() && SelectedComp->AllowReregistration())
			{
				SelectedComp->UnregisterComponent();
			}
			UEditorEngine::CopyPropertiesForUnrelatedObjects(CopiedComponent.Get(), SelectedComp, Options);
			if (!SelectedComp->IsRegistered())
			{
				SelectedComp->RegisterComponent();
			}
		}
		GEditor->EndTransaction();
		FLPrefabEditorModule::PrefabEditor_EndPasteComponentValues.ExecuteIfBound();
	}
	else
	{
		UE_LOG(LPrefabEditor, Error, TEXT("Selected component is missing!"));
	}
}

bool LPrefabEditorTools::HaveValidCopiedActors()
{
	if (CopiedActorPrefabMap.Num() == 0)return false;
	for (auto KeyValuePair : CopiedActorPrefabMap)
	{
		if (!KeyValuePair.Value.IsValid())
		{
			return false;
		}
	}
	return true;
}
bool LPrefabEditorTools::HaveValidCopiedComponent()
{
	return CopiedComponent.IsValid();
}

FString LPrefabEditorTools::PrevSavePrafabFolder = TEXT("");
void LPrefabEditorTools::CreatePrefabAsset()//@todo: make some referenced parameter as override parameter(eg: Actor parameter reference other actor that is not belongs to prefab hierarchy)
{
	auto selectedActor = GetFirstSelectedActor();
	if (!IsValid(selectedActor))
	{
		return;
	}
	if (Cast<ALPrefabLoadHelperActor>(selectedActor) != nullptr || Cast<ALPrefabLevelManagerActor>(selectedActor) != nullptr)
	{
		auto Message = LOCTEXT("CreatePrefabError_PrefabActor", "Cannot create prefab on a LPrefabLoadHelperActor or LPrefabLevelManagerActor!");
		FMessageDialog::Open(EAppMsgType::Ok, Message);
		return;
	}
	auto OldPrefabHelperObject = GetPrefabHelperObject_WhichManageThisActor(selectedActor);
	if (IsValid(OldPrefabHelperObject) && OldPrefabHelperObject->LoadedRootActor == selectedActor)//If create prefab from an existing prefab's root actor, this is not allowed
	{
		auto Message = LOCTEXT("CreatePrefabError_BelongToOtherPrefab", "This actor is a root actor of another prefab, this is not allowed! Instead you can duplicate the prefab asset.");
		FMessageDialog::Open(EAppMsgType::Ok, Message);
		return;
	}
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (DesktopPlatform)
	{
		TArray<FString> OutFileNames;
		DesktopPlatform->SaveFileDialog(
			FSlateApplication::Get().FindBestParentWindowHandleForDialogs(FSlateApplication::Get().GetGameViewport()),
			TEXT("Choose a path to save prefab asset, must inside Content folder"),
			PrevSavePrafabFolder.IsEmpty() ? FPaths::ProjectContentDir() : PrevSavePrafabFolder,
			selectedActor->GetActorLabel() + TEXT("_Prefab"),
			TEXT("*.*"),
			EFileDialogFlags::None,
			OutFileNames
		);
		if (OutFileNames.Num() > 0)
		{
			FString selectedFilePath = OutFileNames[0];
			if (selectedFilePath.StartsWith(FPaths::ProjectContentDir()))
			{
				PrevSavePrafabFolder = FPaths::GetPath(selectedFilePath);
				if (FPaths::FileExists(selectedFilePath + TEXT(".uasset")))
				{
					auto returnValue = FMessageDialog::Open(EAppMsgType::YesNo
						, FText::Format(LOCTEXT("Error_AssetAlreadyExist", "Asset already exist at path: \"{0}\" !\nReplace it?"), FText::FromString(selectedFilePath)));
					if (returnValue == EAppReturnType::No)
					{
						return;
					}
				}
				selectedFilePath.RemoveFromStart(FPaths::ProjectContentDir(), ESearchCase::CaseSensitive);
				FString packageName = TEXT("/Game/") + selectedFilePath;
				UPackage* package = CreatePackage(*packageName);
				if (package == nullptr)
				{
					FMessageDialog::Open(EAppMsgType::Ok
						, LOCTEXT("Error_NotValidPathForSavePrefab", "Selected path not valid, please choose another path to save prefab."));
					return;
				}
				package->FullyLoad();
				FString fileName = FPaths::GetBaseFilename(selectedFilePath);
				auto OutPrefab = NewObject<ULPrefab>(package, ULPrefab::StaticClass(), *fileName, EObjectFlags::RF_Public | EObjectFlags::RF_Standalone);
				FAssetRegistryModule::AssetCreated(OutPrefab);

				auto PrefabHelperObjectWhichManageThisActor = LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(selectedActor);
				if (PrefabHelperObjectWhichManageThisActor == nullptr)//not exist, means in level editor and not create PrefabManagerActor yet, so create it
				{
					auto ManagerActor = ALPrefabLevelManagerActor::GetInstance(selectedActor->GetLevel());
					if (ManagerActor != nullptr)
					{
						PrefabHelperObjectWhichManageThisActor = ManagerActor->PrefabHelperObject;
					}
				}
				check(PrefabHelperObjectWhichManageThisActor != nullptr)
				{
					struct LOCAL
					{
						static auto Make_MapGuidFromParentToSub(const TMap<UObject*, FGuid>& InNewParentMapObjectToGuid, ULPrefabHelperObject* InPrefabHelperObject, const FLSubPrefabData& InOriginSubPrefabData)
						{
							TMap<FGuid, FGuid> Result;
							for (auto& KeyValue : InOriginSubPrefabData.MapObjectGuidFromParentPrefabToSubPrefab)
							{
								auto Object = InPrefabHelperObject->MapGuidToObject[KeyValue.Key];
								if (IsValid(Object))
								{
									auto Guid = InNewParentMapObjectToGuid[Object];
									if (!Result.Contains(Guid))
									{
										Result.Add(Guid, KeyValue.Value);
									}
								}
							}
							return Result;
						}
						static void CollectSubPrefab(AActor* InActor, TMap<TObjectPtr<AActor>, FLSubPrefabData>& InOutSubPrefabMap, ULPrefabHelperObject* InPrefabHelperObject, const TMap<UObject*, FGuid>& InMapObjectToGuid)
						{
							if (InPrefabHelperObject->IsActorBelongsToSubPrefab(InActor))
							{
								auto OriginSubPrefabData = InPrefabHelperObject->GetSubPrefabData(InActor);
								FLSubPrefabData SubPrefabData;
								SubPrefabData.PrefabAsset = OriginSubPrefabData.PrefabAsset;
								SubPrefabData.ObjectOverrideParameterArray = OriginSubPrefabData.ObjectOverrideParameterArray;
								SubPrefabData.MapObjectGuidFromParentPrefabToSubPrefab = Make_MapGuidFromParentToSub(InMapObjectToGuid, InPrefabHelperObject, OriginSubPrefabData);
								InOutSubPrefabMap.Add(InActor, SubPrefabData);
								return;
							}
							TArray<AActor*> ChildrenActors;
							InActor->GetAttachedActors(ChildrenActors);
							for (auto ChildActor : ChildrenActors)
							{
								CollectSubPrefab(ChildActor, InOutSubPrefabMap, InPrefabHelperObject, InMapObjectToGuid);//collect all actor, include subprefab's actor
							}
						}
					};
					TMap<TObjectPtr<AActor>, FLSubPrefabData> SubPrefabMap;
					TMap<UObject*, FGuid> MapObjectToGuid;
					OutPrefab->SavePrefab(selectedActor, MapObjectToGuid, SubPrefabMap);//save prefab first step, just collect guid and sub prefab
					LOCAL::CollectSubPrefab(selectedActor, SubPrefabMap, PrefabHelperObjectWhichManageThisActor, MapObjectToGuid);
					for (auto& KeyValue : SubPrefabMap)
					{
						PrefabHelperObjectWhichManageThisActor->RemoveSubPrefabByAnyActorOfSubPrefab(KeyValue.Key);//remove prefab from origin PrefabHelperObject
					}
					OutPrefab->SavePrefab(selectedActor, MapObjectToGuid, SubPrefabMap);//save prefab second step, store sub prefab data
					OutPrefab->RefreshAgentObjectsInPreviewWorld();

					//make it as subprefab
					TMap<FGuid, TObjectPtr<UObject>> MapGuidToObject;
					for (auto KeyValue : MapObjectToGuid)
					{
						MapGuidToObject.Add(KeyValue.Value, KeyValue.Key);
					}
					PrefabHelperObjectWhichManageThisActor->MakePrefabAsSubPrefab(OutPrefab, selectedActor, MapGuidToObject, {});
					if (auto PrefabManagerActor = ALPrefabLevelManagerActor::GetInstanceByPrefabHelperObject(PrefabHelperObjectWhichManageThisActor))
					{
						PrefabManagerActor->MarkPackageDirty();
					}

					if (OldPrefabHelperObject != nullptr && OldPrefabHelperObject->PrefabAsset != nullptr)
					{
						if (auto PrefabEditor = FLPrefabEditor::GetEditorForPrefabIfValid(OldPrefabHelperObject->PrefabAsset))//if is create prefab inside a prefab editor, then apply the prefab editor
						{
							PrefabEditor->ApplyPrefab();
						}
					}
				}
				CleanupPrefabsInWorld(selectedActor->GetWorld());
			}
			else
			{
				FMessageDialog::Open(EAppMsgType::Ok
					, LOCTEXT("Error_PrefabSaveLocation", "Prefab should only save inside Content folder!"));
			}
		}
	}
}

void LPrefabEditorTools::RefreshLevelLoadedPrefab(ULPrefab* InPrefab)
{
	for (TObjectIterator<ULPrefabHelperObject> Itr; Itr; ++Itr)
	{
		if (Itr->GetIsManagerObject())
		{
			if (!Itr->IsInsidePrefabEditor())
			{
				Itr->CheckPrefabVersion();
			}
		}
	}
}

void LPrefabEditorTools::RefreshOpenedPrefabEditor(ULPrefab* InPrefab)
{
	if (auto PrefabEditor = FLPrefabEditor::GetEditorForPrefabIfValid(InPrefab))//refresh opened prefab
	{
		if (PrefabEditor->GetAnythingDirty())
		{
			auto Msg = LOCTEXT("PrefabEditorChangedDataWillLose", "Prefab editor will automaticallly refresh changed prefab, but detect some data changed in prefab editor, refresh the prefab editor will lose these data, do you want to continue?");
			auto Result = FMessageDialog::Open(EAppMsgType::YesNo, Msg);
			if (Result == EAppReturnType::Yes)
			{
				//reopen this prefab editor
				PrefabEditor->CloseWithoutCheckDataDirty();
				UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
				AssetEditorSubsystem->OpenEditorForAsset(InPrefab);
			}
		}
		else
		{
			//reopen this prefab editor
			PrefabEditor->CloseWithoutCheckDataDirty();
			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			AssetEditorSubsystem->OpenEditorForAsset(InPrefab);
		}
	}
}

void LPrefabEditorTools::RefreshOnSubPrefabChange(ULPrefab* InSubPrefab)
{
	auto AllPrefabs = GetAllPrefabArray();

	struct Local
	{
	public:
		static void RefreshAllPrefabsOnSubPrefabChange(const TArray<ULPrefab*>& InPrefabs, ULPrefab* InSubPrefab)
		{
			for (auto& Prefab : InPrefabs)
			{
				if (Prefab->IsPrefabBelongsToThisSubPrefab(InSubPrefab, false))
				{
					//check if is opened by prefab editor
					if (auto PrefabEditor = FLPrefabEditor::GetEditorForPrefabIfValid(Prefab))//refresh opened prefab
					{
						PrefabEditor->RefreshOnSubPrefabDirty(InSubPrefab);
					}
					else
					{
						//Why comment this? Because we don't need to refresh un-opened prefab, because prefab will reload all sub prefab when open
						//if (Prefab->RefreshOnSubPrefabDirty(InSubPrefab))
						//{
						//	RefreshAllPrefabsOnSubPrefabChange(InPrefabs, Prefab);
						//}
					}
					RefreshAllPrefabsOnSubPrefabChange(InPrefabs, Prefab);
				}
			}
		}
	};

	Local::RefreshAllPrefabsOnSubPrefabChange(AllPrefabs, InSubPrefab);
}

TArray<ULPrefab*> LPrefabEditorTools::GetAllPrefabArray()
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(FName("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Need to do this if running in the editor with -game to make sure that the assets in the following path are available
	TArray<FString> PathsToScan;
	PathsToScan.Add(TEXT("/Game/"));
	AssetRegistry.ScanPathsSynchronous(PathsToScan);

	// Get asset in path
	TArray<FAssetData> ScriptAssetList;
	AssetRegistry.GetAssetsByPath(FName("/Game/"), ScriptAssetList, /*bRecursive=*/true);

	TArray<ULPrefab*> AllPrefabs;
	auto PrefabClassName = ULPrefab::StaticClass()->GetClassPathName();
	// Ensure all assets are loaded
	for (const FAssetData& Asset : ScriptAssetList)
	{
		// Gets the loaded asset, loads it if necessary
		if (Asset.AssetClassPath == PrefabClassName)
		{
			auto AssetObject = Asset.GetAsset();
			if (auto Prefab = Cast<ULPrefab>(AssetObject))
			{
				Prefab->MakeAgentObjectsInPreviewWorld();
				AllPrefabs.Add(Prefab);
			}
		}
	}
	//collect prefabs that are not saved to disc yet
	for (TObjectIterator<ULPrefab> Itr; Itr; ++Itr)
	{
		if (!AllPrefabs.Contains(*Itr))
		{
			AllPrefabs.Add(*Itr);
		}
	}
	return AllPrefabs;
}

void LPrefabEditorTools::UnpackPrefab()
{
	GEditor->BeginTransaction(FText::FromString(TEXT("LGUI UnpackPrefab")));
	auto SelectedActor = GetFirstSelectedActor();
	if (SelectedActor == nullptr)return;
	auto PrefabHelperObject = LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(SelectedActor);
	if (PrefabHelperObject != nullptr)
	{
		check(PrefabHelperObject->SubPrefabMap.Contains(SelectedActor) || PrefabHelperObject->MissingPrefab.Contains(SelectedActor));//should already filtered by menu
		PrefabHelperObject->Modify();
		PrefabHelperObject->RemoveSubPrefabByRootActor(SelectedActor);//the SelectedActor must be root actor, should already filtered by menu
	}
	GEditor->EndTransaction();
	CleanupPrefabsInWorld(SelectedActor->GetWorld());
}

void LPrefabEditorTools::SelectPrefabAsset()
{
	GEditor->BeginTransaction(FText::FromString(TEXT("LGUI SelectPrefabAsset")));
	auto SelectedActor = GetFirstSelectedActor();
	if (SelectedActor == nullptr)return;
	auto PrefabHelperObject = LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(SelectedActor);
	if (PrefabHelperObject != nullptr)
	{
		check(PrefabHelperObject->SubPrefabMap.Contains(SelectedActor));//should have being checked in Browse button
		auto PrefabAsset = PrefabHelperObject->GetSubPrefabAsset(SelectedActor);
		if (IsValid(PrefabAsset))
		{
			TArray<UObject*> ObjectsToSync;
			ObjectsToSync.Add(PrefabAsset);
			GEditor->SyncBrowserToObjects(ObjectsToSync);
		}
	}
	GEditor->EndTransaction();
}

void LPrefabEditorTools::OpenPrefabAsset()
{
	auto SelectedActor = GetFirstSelectedActor();
	if (SelectedActor == nullptr)return;
	auto PrefabHelperObject = LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(SelectedActor);
	if (PrefabHelperObject != nullptr)
	{
		check(PrefabHelperObject->SubPrefabMap.Contains(SelectedActor));//should have being check in menu
		auto PrefabAsset = PrefabHelperObject->GetSubPrefabAsset(SelectedActor);
		if (IsValid(PrefabAsset))
		{
			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			AssetEditorSubsystem->OpenEditorForAsset(PrefabAsset);
		}
	}
}

void LPrefabEditorTools::UpdateLevelPrefab()
{
	auto SelectedActor = GetFirstSelectedActor();
	if (SelectedActor == nullptr)return;
	if (auto PrefabHelperObject = LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(SelectedActor))
	{
		if (auto SubPrefabDataPtr = PrefabHelperObject->SubPrefabMap.Find(SelectedActor))
		{
			PrefabHelperObject->RefreshOnSubPrefabDirty(SubPrefabDataPtr->PrefabAsset, SelectedActor);
		}
	}
}

void LPrefabEditorTools::ToggleLevelPrefabAutoUpdate()
{
	auto SelectedActor = GetFirstSelectedActor();
	if (SelectedActor == nullptr)return;
	if (auto PrefabHelperObject = LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(SelectedActor))
	{
		if (auto SubPrefabDataPtr = PrefabHelperObject->SubPrefabMap.Find(SelectedActor))
		{
			SubPrefabDataPtr->bAutoUpdate = !SubPrefabDataPtr->bAutoUpdate;
		}
	}
}

ULPrefabHelperObject* LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(AActor* InActor)
{
	if (!IsValid(InActor))return nullptr;
	for (TObjectIterator<ULPrefabHelperObject> Itr; Itr; ++Itr)
	{
		if (Itr->IsActorBelongsToThis(InActor))
		{
			return *Itr;
		}
	}
	return nullptr;
}

void LPrefabEditorTools::CleanupPrefabsInWorld(UWorld* World)
{
	for (TActorIterator<ALPrefabLoadHelperActor> ActorItr(World); ActorItr; ++ActorItr)
	{
		auto PrefabActor = *ActorItr;
		if (IsValid(PrefabActor))
		{
			LPrefabUtils::DestroyActorWithHierarchy(PrefabActor, false);
		}
	}
	for (TObjectIterator<ULPrefabHelperObject> Itr; Itr; ++Itr)
	{
		Itr->CleanupInvalidSubPrefab();
	}
}

void LPrefabEditorTools::MakeCurrentLevel(AActor* InActor)
{
	if (IsValid(InActor) && InActor->GetWorld() && InActor->GetLevel())
	{
		if (InActor->GetWorld()->GetCurrentLevel() != InActor->GetLevel())
		{
			if (!InActor->GetWorld()->GetCurrentLevel()->bLocked)
			{
				if (!InActor->GetLevel()->IsCurrentLevel())
				{
					InActor->GetWorld()->SetCurrentLevel(InActor->GetLevel());
				}
			}
			else
			{
				LPrefabUtils::EditorNotification(FText::FromString(FString::Printf(TEXT("The level of selected actor:%s is locked!"), *(InActor->GetActorLabel()))));
			}
		}
	}
}

bool LPrefabEditorTools::IsActorCompatibleWithLGUIToolsMenu(AActor* InActor)
{
	auto ActorClassName = InActor->GetClass()->GetFName();
	if (
		ActorClassName == TEXT("Landscape")
		|| ActorClassName == TEXT("LandscapeStreamingProxy")
		|| ActorClassName == TEXT("WorldDataLayers")
		|| ActorClassName == TEXT("WorldPartitionMiniMap")
		)
	{
		return false;
	}
	return true;
}

void LPrefabEditorTools::ForceGC()
{
	GEngine->ForceGarbageCollection();
}

PRAGMA_ENABLE_OPTIMIZATION

#undef LOCTEXT_NAMESPACE