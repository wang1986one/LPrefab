﻿// Copyright 2019-Present LexLiu. All Rights Reserved.

#if WITH_EDITOR
#include "PrefabSystem/ActorSerializer3.h"
#include "PrefabSystem/LPrefabObjectReaderAndWriter.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "Components/PrimitiveComponent.h"
#include "PrefabSystem/LPrefabManager.h"
#include "LPrefabModule.h"
#if WITH_EDITOR
#include "Tools/UEdMode.h"
#include "LPrefabUtils.h"
#endif

#if LEXPREFAB_CAN_DISABLE_OPTIMIZATION
PRAGMA_DISABLE_OPTIMIZATION
#endif
namespace LPrefabSystem3
{
	void ActorSerializer::SavePrefab(AActor* OriginRootActor, ULPrefab* InPrefab
		, TMap<UObject*, FGuid>& InOutMapObjectToGuid, TMap<TObjectPtr<AActor>, FLSubPrefabData>& InSubPrefabMap
		, bool InForEditorOrRuntimeUse
	)
	{
		if (!OriginRootActor || !InPrefab)
		{
			UE_LOG(LPrefab, Error, TEXT("[ActorSerializer::SerializeActor]OriginRootActor Or InPrefab is null!"));
			return;
		}
		if (!OriginRootActor->GetWorld())
		{
			UE_LOG(LPrefab, Error, TEXT("[ActorSerializer::SerializeActor]Cannot get World from OriginRootActor!"));
			return;
		}
		ActorSerializer serializer;
		serializer.TargetWorld = OriginRootActor->GetWorld();
		for (auto KeyValue : InOutMapObjectToGuid)//Preprocess the map, ignore invalid object
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
		serializer.WriterOrReaderFunctionForSubPrefab = [&serializer](UObject* InObject, TArray<uint8>& InOutBuffer, const TArray<FName>& InOverridePropertyNames) {
			LPrefabSystem::FLPrefabOverrideParameterObjectWriter Writer(InOutBuffer, serializer, InOverridePropertyNames);
			Writer.DoSerialize(InObject);
		};
		serializer.SerializeActor(OriginRootActor, InPrefab);
		InOutMapObjectToGuid = serializer.MapObjectToGuid;
	}

	void ActorSerializer::SerializeActorRecursive(AActor* Actor, FLGUIActorSaveData& OutActorSaveData)
	{
		if (auto SubPrefabDataPtr = SubPrefabMap.Find(Actor))//sub prefab
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
				WriterOrReaderFunctionForSubPrefab(SubPrefabObject, SubPrefabOverrideData, DataItem.MemberPropertyNames);
				FLPrefabOverrideParameterRecordData RecordDataItem;
				RecordDataItem.ObjectGuid = MapObjectToGuid[SubPrefabObject];
				RecordDataItem.OverrideParameterData = SubPrefabOverrideData;
				RecordDataItem.OverrideParameterNames = DataItem.MemberPropertyNames;
				OutActorSaveData.ObjectOverrideParameterArray.Add(RecordDataItem);
			}
		}
		else
		{
			auto ActorGuid = MapObjectToGuid[Actor];

			OutActorSaveData.ActorClass = FindOrAddClassFromList(Actor->GetClass());
			OutActorSaveData.ActorGuid = ActorGuid;
			OutActorSaveData.ObjectFlags = (uint32)Actor->GetFlags();
			WriterOrReaderFunction(Actor, OutActorSaveData.ActorPropertyData, false);
			if (auto RootComp = Actor->GetRootComponent())
			{
				OutActorSaveData.RootComponentGuid = MapObjectToGuid[RootComp];
			}
			TArray<UObject*> DefaultSubObjects;
			Actor->CollectDefaultSubobjects(DefaultSubObjects, true);
			for (auto DefaultSubObject : DefaultSubObjects)
			{
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
				SerializeActorRecursive(ChildActor, ChildActorSaveData);
				ChildSaveDataList.Add(ChildActorSaveData);
			}
			OutActorSaveData.ChildActorData = ChildSaveDataList;
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
		SerializeActorRecursive(OriginRootActor, OutData.SavedActor);
		//serialize objects and components
		SerializeObjectArray(OutData.SavedObjects, OutData.SavedComponents);
	}
	void ActorSerializer::SerializeActor(AActor* OriginRootActor, ULPrefab* InPrefab)
	{
		auto StartTime = FDateTime::Now();

		FLPrefabSaveData SaveData;
		SerializeActorToData(OriginRootActor, SaveData);

		FBufferArchive ToBinary;
		ToBinary << SaveData;

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
			InPrefab->CreateTime = FDateTime::Now();

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

	void ActorSerializer::SerializeObjectArray(TArray<FLGUIObjectSaveData>& ObjectSaveDataArray, TArray<FLGUIComponentSaveData>& ComponentSaveDataArray)
	{
		for (int i = 0; i < WillSerializeObjectArray.Num(); i++)
		{
			auto Object = WillSerializeObjectArray[i];
			auto Class = Object->GetClass();
			if (Class->IsChildOf(UActorComponent::StaticClass()))
			{
				FLGUIComponentSaveData ComponentSaveDataItem;
				ComponentSaveDataItem.ComponentClass = FindOrAddClassFromList(Class);
				ComponentSaveDataItem.ComponentName = Object->GetFName();
				ComponentSaveDataItem.ComponentGuid = MapObjectToGuid[Object];
				ComponentSaveDataItem.ObjectFlags = (uint32)Object->GetFlags();
				ComponentSaveDataItem.OuterObjectGuid = MapObjectToGuid[Object->GetOuter()];
				if (auto SceneComp = Cast<USceneComponent>(Object))
				{
					if (auto ParentComp = SceneComp->GetAttachParent())
					{
						if (WillSerializeActorArray.Contains(ParentComp->GetOwner()))//check if parent component belongs to this prefab
						{
							ComponentSaveDataItem.SceneComponentParentGuid = MapObjectToGuid[ParentComp];//@todo: better way to store SceneComponent's parent?
						}
					}
					WriterOrReaderFunction(Object, ComponentSaveDataItem.PropertyData, true);
				}
				else
				{
					WriterOrReaderFunction(Object, ComponentSaveDataItem.PropertyData, false);
				}
				ComponentSaveDataArray.Add(ComponentSaveDataItem);
			}
			else
			{
				FLGUIObjectSaveData ObjectSaveDataItem;
				ObjectSaveDataItem.ObjectClass = FindOrAddClassFromList(Class);
				ObjectSaveDataItem.ObjectGuid = MapObjectToGuid[Object];
				ObjectSaveDataItem.ObjectFlags = (uint32)Object->GetFlags();
				ObjectSaveDataItem.OuterObjectGuid = MapObjectToGuid[Object->GetOuter()];
				WriterOrReaderFunction(Object, ObjectSaveDataItem.PropertyData, false);
				ObjectSaveDataArray.Add(ObjectSaveDataItem);
			}
		}
	}
}
#if LEXPREFAB_CAN_DISABLE_OPTIMIZATION
PRAGMA_ENABLE_OPTIMIZATION
#endif

#endif
