﻿// Copyright 2019-Present LexLiu. All Rights Reserved.

#if WITH_EDITOR
#include "PrefabSystem/ActorSerializer6.h"
#include "PrefabSystem/LPrefabObjectReaderAndWriter.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "Components/PrimitiveComponent.h"
#include "PrefabSystem/LPrefabManager.h"
#include "LPrefabModule.h"
#include "Misc/NetworkVersion.h"
#include "Runtime/Launch/Resources/Version.h"
#if WITH_EDITOR
#include "Tools/UEdMode.h"
#include "LPrefabUtils.h"
#endif

#if LEXPREFAB_CAN_DISABLE_OPTIMIZATION
PRAGMA_DISABLE_OPTIMIZATION
#endif
namespace LPrefabSystem6
{
	void ActorSerializer::SavePrefab(AActor* OriginRootActor, ULPrefab* InPrefab
		, TMap<UObject*, FGuid>& InOutMapObjectToGuid, TMap<TObjectPtr<AActor>, FLSubPrefabData>& InSubPrefabMap
		, bool InForEditorOrRuntimeUse
	)
	{
		if (!OriginRootActor || !InPrefab)
		{
			UE_LOG(LPrefab, Error, TEXT("[%s].%d OriginRootActor Or InPrefab is null!"), ANSI_TO_TCHAR(__FUNCTION__), __LINE__);
			return;
		}
		if (!IsValid(OriginRootActor))
		{
			UE_LOG(LPrefab, Error, TEXT("[%s].%d OriginRootActor is not valid!"), ANSI_TO_TCHAR(__FUNCTION__), __LINE__);
			return;
		}
		if (!OriginRootActor->GetWorld())
		{
			UE_LOG(LPrefab, Error, TEXT("[%s].%d Cannot get World from OriginRootActor!"), ANSI_TO_TCHAR(__FUNCTION__), __LINE__);
			return;
		}
		if (OriginRootActor->HasAnyFlags(EObjectFlags::RF_Transient))
		{
			UE_LOG(LPrefab, Error, TEXT("[%s].%d OriginRootActor is transient!"), ANSI_TO_TCHAR(__FUNCTION__), __LINE__);
			return;
		}
		if (!InForEditorOrRuntimeUse && OriginRootActor->IsEditorOnly())
		{
			UE_LOG(LPrefab, Error, TEXT("[%s].%d OriginRootActor is editor only!"), ANSI_TO_TCHAR(__FUNCTION__), __LINE__);
			return;
		}
		ActorSerializer serializer;
		serializer.TargetWorld = OriginRootActor->GetWorld();
		for (auto& KeyValue : InOutMapObjectToGuid)//Preprocess the map, ignore invalid object
		{
			if (IsValid(KeyValue.Key))
			{
				serializer.MapObjectToGuid.Add(KeyValue.Key, KeyValue.Value);
			}
		}
		serializer.SubPrefabMap = InSubPrefabMap;
		serializer.bIsEditorOrRuntime = InForEditorOrRuntimeUse;
		serializer.WriterOrReaderFunction = [&serializer](UObject* InObject, TArray<uint8>& InOutBuffer, bool InIsSceneComponent) {
			auto ExcludeProperties = InIsSceneComponent ? serializer.GetSceneComponentExcludeProperties() : TSet<FName>();
			LPrefabSystem::FLPrefabObjectWriter Writer(InOutBuffer, serializer, ExcludeProperties);
			Writer.DoSerialize(InObject);
		};
		serializer.WriterOrReaderFunctionForSubPrefabOverride = [&serializer](UObject* InObject, TArray<uint8>& InOutBuffer, const TArray<FName>& InOverridePropertyNames) {
			LPrefabSystem::FLPrefabOverrideParameterObjectWriter Writer(InOutBuffer, serializer, InOverridePropertyNames);
			Writer.DoSerialize(InObject);
		};
		serializer.SerializeActor(OriginRootActor, InPrefab);
		InOutMapObjectToGuid = serializer.MapObjectToGuid;
	}

	void ActorSerializer::SerializeActorRecursive(AActor* Actor, FLGUIActorSaveData& OutActorSaveData, TMap<FGuid, TArray<uint8>>& SavedObjectData)
	{
		if (!IsValid(Actor))return;
		if (auto SubPrefabDataPtr = SubPrefabMap.Find(Actor))//sub prefab's actor is not collected in WillSerailizeActorArray
		{
			OutActorSaveData.bIsPrefab = true;
			OutActorSaveData.PrefabAssetIndex = FindOrAddAssetIdFromList(SubPrefabDataPtr->PrefabAsset);
			OutActorSaveData.ActorGuid = MapObjectToGuid[Actor];
			OutActorSaveData.MapObjectGuidFromParentPrefabToSubPrefab = SubPrefabDataPtr->MapObjectGuidFromParentPrefabToSubPrefab;

			//serialize override parameter data
			for (auto& DataItem : SubPrefabDataPtr->ObjectOverrideParameterArray)
			{
				TArray<uint8> SubPrefabOverrideData;
				auto SubPrefabObject = DataItem.Object.Get();
				if (MapObjectToGuid.Contains(SubPrefabObject))
				{
					FLPrefabOverrideParameterSaveData RecordDataItem;
					RecordDataItem.OverrideParameterNames = DataItem.MemberPropertyNames;
					WriterOrReaderFunctionForSubPrefabOverride(SubPrefabObject, RecordDataItem.OverrideParameterData, DataItem.MemberPropertyNames);
					OutActorSaveData.MapObjectGuidToSubPrefabOverrideParameter.Add(MapObjectToGuid[SubPrefabObject], RecordDataItem);
				}
			}
		}
		else
		{
			if (!WillSerializeActorArray.Contains(Actor))return;
			auto ActorGuid = MapObjectToGuid[Actor];

			OutActorSaveData.ObjectClass = FindOrAddClassFromList(Actor->GetClass());
			OutActorSaveData.ActorGuid = ActorGuid;
			OutActorSaveData.ObjectFlags = (uint32)Actor->GetFlags();
			WriterOrReaderFunction(Actor, SavedObjectData.Add(ActorGuid), false);
			if (auto RootComp = Actor->GetRootComponent())
			{
				OutActorSaveData.RootComponentGuid = MapObjectToGuid[RootComp];
			}
			TArray<UObject*> DefaultSubObjects;
			Actor->CollectDefaultSubobjects(DefaultSubObjects);
			for (auto DefaultSubObject : DefaultSubObjects)
			{
				FGuid DefaultSubObjectGuid;
				if (!CollectObjectToSerailize(DefaultSubObject, DefaultSubObjectGuid))continue;
				OutActorSaveData.DefaultSubObjectGuidArray.Add(MapObjectToGuid[DefaultSubObject]);
				OutActorSaveData.DefaultSubObjectNameArray.Add(DefaultSubObject->GetFName());
			}

			TArray<AActor*> ChildrenActors;
			Actor->GetAttachedActors(ChildrenActors);
#if WITH_EDITOR
			if (!ULPrefabManagerObject::Serialize_SortChildrenActors.ExecuteIfBound(ChildrenActors))
			{
				Algo::Sort(ChildrenActors, [](const AActor* A, const AActor* B) {
					//sort on ActorLabel so the Tick function can be predictable because deserialize order is determinate.
					return A->GetActorLabel().Compare(B->GetActorLabel()) < 0;//compare name for normal actor
					});
			}
#endif
			TArray<FLGUIActorSaveData> ChildSaveDataList;
			for (auto ChildActor : ChildrenActors)
			{
				FLGUIActorSaveData ChildActorSaveData;
				SerializeActorRecursive(ChildActor, ChildActorSaveData, SavedObjectData);
				ChildSaveDataList.Add(ChildActorSaveData);
			}
			OutActorSaveData.ChildrenActorDataArray = ChildSaveDataList;
		}
	}
	void ActorSerializer::SerializeActorToData(AActor* OriginRootActor, FLPrefabSaveData& OutData)
	{
		if (LPrefabManager == nullptr)
		{
			LPrefabManager = ULPrefabWorldSubsystem::GetInstance(OriginRootActor->GetWorld());
		}
		CollectActorRecursive(OriginRootActor);
		//serailize actor
		SerializeActorRecursive(OriginRootActor, OutData.SavedActor, OutData.SavedObjectData);
		//serialize objects and components
		SerializeObjectArray(OutData.SavedObjects, OutData.SavedObjectData, OutData.MapSceneComponentToParent);
	}
	void ActorSerializer::SerializeActor(AActor* OriginRootActor, ULPrefab* InPrefab)
	{

		auto StartTime = FDateTime::Now();

		FLPrefabSaveData SaveData;
		SerializeActorToData(OriginRootActor, SaveData);

		FBufferArchive ToBinary;
#if WITH_EDITOR
		if (bIsEditorOrRuntime)
		{
			FStructuredArchiveFromArchive(ToBinary).GetSlot() << SaveData;
		}
		else
#endif
		{
			ToBinary << SaveData;
		}

		if (ToBinary.Num() <= 0)
		{
			UE_LOG(LPrefab, Warning, TEXT("Save binary length is 0!"));
			return;
		}
#if WITH_EDITOR
		if (bIsEditorOrRuntime)
		{
			InPrefab->BinaryData = ToBinary;
			InPrefab->ThumbnailDirty = true;
			InPrefab->CreateTime = FDateTime::UtcNow();

			//clear old reference data
			InPrefab->ReferenceAssetList.Empty();
			InPrefab->ReferenceClassList.Empty();
			InPrefab->ReferenceNameList.Empty();
			InPrefab->ReferenceTextList.Empty();
			InPrefab->ReferenceStringList.Empty();
			//fill new reference data
			InPrefab->ReferenceAssetList = this->ReferenceAssetList;
			InPrefab->ReferenceClassList = this->ReferenceClassList;
			InPrefab->ReferenceNameList = this->ReferenceNameList;

			InPrefab->ArchiveVersion = GPackageFileUEVersion.FileVersionUE4;
			InPrefab->ArchiveVersionUE5 = GPackageFileUEVersion.FileVersionUE5;
			InPrefab->ArchiveLicenseeVer = GPackageFileLicenseeUEVersion;
			InPrefab->ArEngineNetVer = FNetworkVersion::GetEngineNetworkProtocolVersion();
			InPrefab->ArGameNetVer = FNetworkVersion::GetGameNetworkProtocolVersion();

			InPrefab->MarkPackageDirty();
		}
		else
#endif
		{
			InPrefab->BinaryDataForBuild = ToBinary;

			//fill new reference data
			InPrefab->ReferenceAssetListForBuild = this->ReferenceAssetList;
			InPrefab->ReferenceClassListForBuild = this->ReferenceClassList;
			InPrefab->ReferenceNameListForBuild = this->ReferenceNameList;

			InPrefab->ArchiveVersion_ForBuild = GPackageFileUEVersion.FileVersionUE4;
			InPrefab->ArchiveVersionUE5_ForBuild = GPackageFileUEVersion.FileVersionUE5;
			InPrefab->ArchiveLicenseeVer_ForBuild = GPackageFileLicenseeUEVersion;
			InPrefab->ArEngineNetVer_ForBuild = FNetworkVersion::GetEngineNetworkProtocolVersion();
			InPrefab->ArGameNetVer_ForBuild = FNetworkVersion::GetGameNetworkProtocolVersion();
		}

		InPrefab->EngineMajorVersion = ENGINE_MAJOR_VERSION;
		InPrefab->EngineMinorVersion = ENGINE_MINOR_VERSION;
		InPrefab->PrefabVersion = LPREFAB_CURRENT_VERSION;

		auto TimeSpan = FDateTime::Now() - StartTime;
		UE_LOG(LPrefab, Log, TEXT("Take %fs saving prefab: %s"), TimeSpan.GetTotalSeconds(), *InPrefab->GetName());
	}

	void ActorSerializer::CollectActorRecursive(AActor* Actor)
	{
		if (!IsValid(Actor))return;
		if (Actor->HasAnyFlags(EObjectFlags::RF_Transient))return;
#if WITH_EDITOR
		/** 
		 * Since LPrefab not work well with ActorBlueprint, so we give a hint if detect ActorBlueprint.
		 */
		auto ActorClass = Actor->GetClass();
		if (ActorClass->ClassGeneratedBy != nullptr && ActorClass->HasAnyClassFlags(EClassFlags::CLASS_CompiledFromBlueprint))
		{
			auto MsgText = FText::Format(NSLOCTEXT("LGUIActorSerializer6", "Warning_ActorBlueprintInPrefab", "Trying to create a prefab with ActorBlueprint '{0}', ActorBlueprint not work well with PrefabEditor, suggest to use native Actor."), FText::FromString(Actor->GetActorLabel()));
			if (bIsEditorOrRuntime)
			{
				LPrefabUtils::EditorNotification(MsgText, 10.0f);
			}
			UE_LOG(LPrefab, Warning, TEXT("%s"), *MsgText.ToString());
		}
		if (bIsEditorOrRuntime)
		{
		}
		else
#endif
		{
			if (Actor->bIsEditorOnlyActor)return;
		}
		//collect actor
		if (!SubPrefabMap.Contains(Actor))//sub prefab's actor should not put to the list
		{
			WillSerializeActorArray.Add(Actor);
		}
		if (!MapObjectToGuid.Contains(Actor))
		{
			MapObjectToGuid.Add(Actor, FGuid::NewGuid());
		}

		TArray<AActor*> ChildrenActors;
		Actor->GetAttachedActors(ChildrenActors);
		for (auto ChildActor : ChildrenActors)
		{
			CollectActorRecursive(ChildActor);//collect all actor, include subprefab's actor
		}
	}

	void ActorSerializer::SerializeObjectArray(TMap<FGuid, FLGUIObjectSaveData>& ObjectSaveDataArray, TMap<FGuid, TArray<uint8>>& SavedObjectData, TMap<FGuid, FGuid>& MapSceneComponentToParent)
	{
		for (int i = 0; i < WillSerializeObjectArray.Num(); i++)
		{
			auto Object = WillSerializeObjectArray[i];
			auto Class = Object->GetClass();
			FLGUIObjectSaveData ObjectSaveDataItem;
			ObjectSaveDataItem.ObjectClass = FindOrAddClassFromList(Class);
			ObjectSaveDataItem.ObjectName = Object->GetFName();
			ObjectSaveDataItem.ObjectFlags = (uint32)Object->GetFlags();
			ObjectSaveDataItem.OuterObjectGuid = MapObjectToGuid[Object->GetOuter()];
			auto SceneComp = Cast<USceneComponent>(Object);
			if (SceneComp)
			{
				if (auto ParentComp = SceneComp->GetAttachParent())
				{
					if (WillSerializeActorArray.Contains(ParentComp->GetOwner()))//check if parent component belongs to this prefab
					{
						MapSceneComponentToParent.Add(MapObjectToGuid[Object], MapObjectToGuid[ParentComp]);
					}
				}
			}
			WriterOrReaderFunction(Object, SavedObjectData.Add(MapObjectToGuid[Object]), SceneComp != nullptr);
			TArray<UObject*> DefaultSubObjects;
			Object->CollectDefaultSubobjects(DefaultSubObjects);
			for (auto DefaultSubObject : DefaultSubObjects)
			{
				FGuid DefaultSubObjectGuid;
				if (!CollectObjectToSerailize(DefaultSubObject, DefaultSubObjectGuid))continue;
				ObjectSaveDataItem.DefaultSubObjectGuidArray.Add(MapObjectToGuid[DefaultSubObject]);
				ObjectSaveDataItem.DefaultSubObjectNameArray.Add(DefaultSubObject->GetFName());
			}
			ObjectSaveDataArray.Add(MapObjectToGuid[Object], ObjectSaveDataItem);
		}
	}
}
#if LEXPREFAB_CAN_DISABLE_OPTIMIZATION
PRAGMA_ENABLE_OPTIMIZATION
#endif

#endif