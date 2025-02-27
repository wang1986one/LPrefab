﻿// Copyright 2019-Present LexLiu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"

class UTexture2D;

class LPREFAB_API LPrefabUtils
{
public:	
	/** Destroy actor and all it's hierarchy children */
	static void DestroyActorWithHierarchy(AActor* Target, bool WithHierarchy = true);
	//Find first component of type T from InActor, if not found go up hierarchy until found
	template<class T>
	static T* GetComponentInParent(AActor* InActor, bool IncludeUnregisteredComponent = true)
	{
		static_assert(TPointerIsConvertibleFromTo<T, const UActorComponent>::Value, "'T' template parameter to GetComponentInParent must be derived from UActorComponent");
		T* resultComp = nullptr;
		AActor* parentActor = InActor;
		while (IsValid(parentActor))
		{
			resultComp = parentActor->FindComponentByClass<T>();
			if (IsValid(resultComp))
			{
				if (resultComp->IsRegistered())
				{
					return resultComp;
				}
				else
				{
					if (IncludeUnregisteredComponent)
					{
						return resultComp;
					}
				}
			}
			parentActor = parentActor->GetAttachParentActor();
		}
		return nullptr;
	}
	//Collect all component of type T from children
	template<class T>
	static TArray<T*> GetComponentsInChildren(AActor* InActor, bool IncludeSelf = false)
	{
		static_assert(TPointerIsConvertibleFromTo<T, const UActorComponent>::Value, "'T' template parameter to GetComponentsInChildren must be derived from UActorComponent");
		TArray<T*> result;
		if (!IsValid(InActor))
		{
			UE_LOG(LogTemp, Error, TEXT("[LPrefabUtils::GetComponentsInChildren]InActor is not valid!"));
			return result;
		}
		if (IncludeSelf)
		{
			CollectComponentsInChildrenRecursive(InActor, result);
		}
		else
		{
			TArray<AActor*> childrenActors;
			InActor->GetAttachedActors(childrenActors);
			if (childrenActors.Num() > 0)
			{
				for (AActor* actor : childrenActors)
				{
					CollectComponentsInChildrenRecursive(actor, result);
				}
			}
		}
		return result;
	}
	template<class T>
	static T* GetComponentInChildren(AActor* InActor, bool IncludeSelf = false)
	{
		static_assert(TPointerIsConvertibleFromTo<T, const UActorComponent>::Value, "'T' template parameter to GetComponentsInChildren must be derived from UActorComponent");
		if (!IsValid(InActor))
		{
			UE_LOG(LogTemp, Error, TEXT("[LPrefabUtils::GetComponentsInChildren]InActor is not valid!"));
			return nullptr;
		}
		if (IncludeSelf)
		{
			return GetComponentInChildrenRecursive<T>(InActor);
		}
		else
		{
			TArray<AActor*> childrenActors;
			InActor->GetAttachedActors(childrenActors);
			if (childrenActors.Num() > 0)
			{
				for (AActor* actor : childrenActors)
				{
					if (auto result = GetComponentInChildrenRecursive<T>(actor))
					{
						return result;
					}
				}
			}
		}
		return nullptr;
	}
#if WITH_EDITOR
	//nodify some informations in editor
	static void EditorNotification(FText NotifyText, float ExpireDuration = 5.0f);
#endif
	/**
	 * Collect children actors reculsively.
	 * @param	Target	Search from this actor.
	 * @param	AllChildrenActors	Result children actor array.
	 * @param	IncludeTarget	Should include Target actor in result array?
	 */
	static void CollectChildrenActors(AActor* Target, TArray<AActor*>& AllChildrenActors, bool IncludeTarget = true);

	static TArray<uint8> GetMD5(const FString& InString);
	static FString GetMD5String(const FString& InString);
#if WITH_EDITOR
	static void NotifyPropertyChanged(UObject* Object, FProperty* Property);
	static void NotifyPropertyChanged(UObject* Object, FName PropertyName);
	static void NotifyPropertyPreChange(UObject* Object, FProperty* Property);
	static void NotifyPropertyPreChange(UObject* Object, FName PropertyName);
#endif

	static void LogObjectFlags(UObject* obj);
	static void LogClassFlags(UClass* cls);
private:
	template<class T>
	static void CollectComponentsInChildrenRecursive(AActor* InActor, TArray<T*>& InOutArray)
	{
		auto& components = InActor->GetComponents();
		for (UActorComponent* comp : components)
		{
			if (comp->IsA(T::StaticClass()))
			{
				InOutArray.Add((T*)comp);
			}
		}

		TArray<AActor*> childrenActors;
		InActor->GetAttachedActors(childrenActors);
		if (childrenActors.Num() > 0)
		{
			for (AActor* actor : childrenActors)
			{
				CollectComponentsInChildrenRecursive(actor, InOutArray);
			}
		}
	}
	template<class T>
	static T* GetComponentInChildrenRecursive(AActor* InActor)
	{
		auto& components = InActor->GetComponents();
		for (UActorComponent* comp : components)
		{
			if (comp->IsA(T::StaticClass()))
			{
				return (T*)comp;
			}
		}

		TArray<AActor*> childrenActors;
		InActor->GetAttachedActors(childrenActors);
		if (childrenActors.Num() > 0)
		{
			for (AActor* actor : childrenActors)
			{
				if (auto result = GetComponentInChildrenRecursive<T>(actor))
				{
					return result;
				}
			}
		}
		return nullptr;
	}
};