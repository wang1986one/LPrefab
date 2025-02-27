// Copyright 2019-Present LexLiu. All Rights Reserved.

#include "PrefabAnimation/LPrefabSequenceObjectReference.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/Blueprint.h"
#include "UObject/Package.h"
#include "LPrefabModule.h"

#if LEXPREFAB_CAN_DISABLE_OPTIMIZATION
PRAGMA_DISABLE_OPTIMIZATION
#endif

#if WITH_EDITOR
FString FLPrefabSequenceObjectReference::GetActorPathRelativeToContextActor(AActor* InContextActor, AActor* InActor)
{
	if (InActor == InContextActor)
	{
		return TEXT("/");
	}
	else if (InActor->IsAttachedTo(InContextActor))
	{
		FString Result = InActor->GetActorLabel();
		AActor* Parent = InActor->GetAttachParentActor();
		while (Parent != nullptr && Parent != InContextActor)
		{
			Result = Parent->GetActorLabel() + "/" + Result;
			Parent = Parent->GetAttachParentActor();
		}
		return Result;
	}
	return TEXT("");
}
AActor* FLPrefabSequenceObjectReference::GetActorFromContextActorByRelativePath(AActor* InContextActor, const FString& InPath)
{
	if (InPath == TEXT("/"))
	{
		return InContextActor;
	}
	else
	{
		TArray<FString> SplitedArray;
		{
			if (InPath.Contains(TEXT("/")))
			{
				FString SourceString = InPath;
				FString Left, Right;
				TArray<AActor*> ChildrenActors;
				while (SourceString.Split(TEXT("/"), &Left, &Right, ESearchCase::CaseSensitive))
				{
					SplitedArray.Add(Left);
					SourceString = Right;
				}
				SplitedArray.Add(Right);
			}
			else
			{
				SplitedArray.Add(InPath);
			}
		}

		AActor* ParentActor = InContextActor;
		TArray<AActor*> ChildrenActors;
		for (int i = 0; i < SplitedArray.Num(); i++)
		{
			auto& PathItem = SplitedArray[i];
			ParentActor->GetAttachedActors(ChildrenActors);
			AActor* FoundChildActor = nullptr;
			for (auto& ChildActor : ChildrenActors)
			{
				if (PathItem == ChildActor->GetActorLabel())
				{
					FoundChildActor = ChildActor;
					break;
				}
			}
			if (FoundChildActor != nullptr)
			{
				if (i + 1 == SplitedArray.Num())
				{
					return FoundChildActor;
				}
				ParentActor = FoundChildActor;
			}
			else
			{
				return nullptr;
			}
		}
	}
	return nullptr;
}
bool FLPrefabSequenceObjectReference::FixObjectReferenceFromEditorHelpers(AActor* InContextActor)
{
	if (auto FoundHelperActor = GetActorFromContextActorByRelativePath(InContextActor, this->HelperActorPath))
	{
		HelperActor = FoundHelperActor;
		HelperActorLabel = HelperActor->GetActorLabel();
		if (HelperClass == AActor::StaticClass())
		{
			Object = HelperActor;
			return true;
		}
		else if (HelperClass->IsChildOf(UActorComponent::StaticClass()))
		{
			TArray<UActorComponent*> Components;
			HelperActor->GetComponents(HelperClass, Components);
			if (Components.Num() == 1)
			{
				Object = Components[0];
				return true;
			}
			else if (Components.Num() > 1)
			{
				for (auto& CompItem : Components)
				{
					if (CompItem->GetFName() == HelperComponentName)
					{
						Object = CompItem;
						break;
					}
				}
				if (Object == nullptr)
				{
					Object = Components[0];
				}
				return true;
			}
		}
	}
	return false;
}
bool FLPrefabSequenceObjectReference::CanFixObjectReferenceFromEditorHelpers()const
{
	return IsValid(HelperClass)
		&& !HelperComponentName.IsNone()
		&& !HelperActorPath.IsEmpty()
		;
}
bool FLPrefabSequenceObjectReference::IsObjectReferenceGood(AActor* InContextActor)const
{
	CheckTargetObject();
	AActor* Actor = Cast<AActor>(Object);
	if (Actor == nullptr)
	{
		if (auto Component = Cast<UActorComponent>(Object))
		{
			Actor = Component->GetOwner();
		}
	}

	if (Actor != nullptr)
	{
		return Actor->GetLevel() == InContextActor->GetLevel()
			&& (Actor == InContextActor || Actor->IsAttachedTo(InContextActor))//only allow actor self or child actor
			;
	}
	return false;
}
bool FLPrefabSequenceObjectReference::IsEditorHelpersGood(AActor* InContextActor)const
{
	return IsValid(HelperActor)
		&& IsValid(HelperClass)
		&& !HelperComponentName.IsNone()
		&& HelperActorPath == GetActorPathRelativeToContextActor(InContextActor, HelperActor)
		;
}
#endif

bool FLPrefabSequenceObjectReference::InitHelpers(AActor* InContextActor)
{
	if (auto Actor = Cast<AActor>(Object))
	{
		this->HelperActor = Actor;
		this->HelperClass = AActor::StaticClass();
		this->HelperComponentName = TEXT("Actor");
#if WITH_EDITOR
		this->HelperActorLabel = Actor->GetActorLabel();
		this->HelperActorPath = GetActorPathRelativeToContextActor(InContextActor, Actor);
#endif
		return true;
	}
	else
	{
		if (auto Component = Cast<UActorComponent>(Object))
		{
			Actor = Component->GetOwner();
			this->HelperActor = Actor;
			this->HelperClass = Component->GetClass();
			this->HelperComponentName = Component->GetFName();
#if WITH_EDITOR
			this->HelperActorLabel = Actor->GetActorLabel();
			this->HelperActorPath = GetActorPathRelativeToContextActor(InContextActor, Actor);
#endif
			return true;
		}
	}
	return false;
}
bool FLPrefabSequenceObjectReference::CreateForObject(AActor* InContextActor, UObject* InObject, FLPrefabSequenceObjectReference& OutResult)
{
	OutResult.Object = InObject;
	return OutResult.InitHelpers(InContextActor);
}

bool FLPrefabSequenceObjectReference::CheckTargetObject()const
{
	if (IsValid(Object))
	{
		return true;
	}
	else
	{
		if (IsValid(HelperActor) && IsValid(HelperClass))
		{
			if (HelperClass == AActor::StaticClass())
			{
				Object = HelperActor;
				return true;
			}
			else
			{
				TArray<UActorComponent*> Components;
				HelperActor->GetComponents(HelperClass, Components);
				if (Components.Num() == 1)
				{
					Object = Components[0];
					return true;
				}
				else if (Components.Num() > 1)
				{
					for (auto Comp : Components)
					{
						if (Comp->GetFName() == HelperComponentName)
						{
							Object = Comp;
							return true;
						}
					}
				}
			}
		}
	}
	return false;
}

UObject* FLPrefabSequenceObjectReference::Resolve() const
{
	CheckTargetObject();
	return Object;
}

bool FLPrefabSequenceObjectReferenceMap::HasBinding(const FGuid& ObjectId) const
{
	return BindingIds.Contains(ObjectId);
}

void FLPrefabSequenceObjectReferenceMap::RemoveBinding(const FGuid& ObjectId)
{
	int32 Index = BindingIds.IndexOfByKey(ObjectId);
	if (Index != INDEX_NONE)
	{
		BindingIds.RemoveAtSwap(Index, 1, false);
		References.RemoveAtSwap(Index, 1, false);
	}
}

void FLPrefabSequenceObjectReferenceMap::CreateBinding(const FGuid& ObjectId, const FLPrefabSequenceObjectReference& ObjectReference)
{
	int32 ExistingIndex = BindingIds.IndexOfByKey(ObjectId);
	if (ExistingIndex == INDEX_NONE)
	{
		ExistingIndex = BindingIds.Num();

		BindingIds.Add(ObjectId);
		References.AddDefaulted();
	}

	References[ExistingIndex].Array.AddUnique(ObjectReference);
}

void FLPrefabSequenceObjectReferenceMap::ResolveBinding(const FGuid& ObjectId, TArray<UObject*, TInlineAllocator<1>>& OutObjects) const
{
	int32 Index = BindingIds.IndexOfByKey(ObjectId);
	if (Index == INDEX_NONE)
	{
		return;
	}

	for (const FLPrefabSequenceObjectReference& Reference : References[Index].Array)
	{
		if (UObject* Object = Reference.Resolve())
		{
			OutObjects.Add(Object);
		}
	}
}

#if WITH_EDITOR
bool FLPrefabSequenceObjectReferenceMap::IsObjectReferencesGood(AActor* InContextActor)const
{
	for (auto& Reference : References)
	{
		for (auto& RefItem : Reference.Array)
		{
			if (!RefItem.IsObjectReferenceGood(InContextActor))
			{
				return false;
			}
		}
	}
	return true;
}
bool FLPrefabSequenceObjectReferenceMap::IsEditorHelpersGood(AActor* InContextActor)const
{
	for (auto& Reference : References)
	{
		for (auto& RefItem : Reference.Array)
		{
			if (!RefItem.IsEditorHelpersGood(InContextActor))
			{
				return false;
			}
		}
	}
	return true;
}
bool FLPrefabSequenceObjectReferenceMap::FixObjectReferences(AActor* InContextActor)
{
	bool anythingChanged = false;
	for (auto& Reference : References)
	{
		for (auto& RefItem : Reference.Array)
		{
			if (!RefItem.IsObjectReferenceGood(InContextActor) && RefItem.CanFixObjectReferenceFromEditorHelpers())
			{
				if (RefItem.FixObjectReferenceFromEditorHelpers(InContextActor))
				{
					anythingChanged = true;
				}
			}
		}
	}
	return anythingChanged;
}
bool FLPrefabSequenceObjectReferenceMap::FixEditorHelpers(AActor* InContextActor)
{
	bool anythingChanged = false;
	for (auto& Reference : References)
	{
		for (auto& RefItem : Reference.Array)
		{
			if (RefItem.IsObjectReferenceGood(InContextActor) && !RefItem.IsEditorHelpersGood(InContextActor))
			{
				if (RefItem.InitHelpers(InContextActor))
				{
					anythingChanged = true;
				}
			}
		}
	}
	return anythingChanged;
}
#endif

#if LEXPREFAB_CAN_DISABLE_OPTIMIZATION
PRAGMA_ENABLE_OPTIMIZATION
#endif
