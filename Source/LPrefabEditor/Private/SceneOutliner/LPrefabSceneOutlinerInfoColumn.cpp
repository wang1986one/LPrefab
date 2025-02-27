﻿// Copyright 2019-Present LexLiu. All Rights Reserved.

#include "SceneOutliner/LPrefabSceneOutlinerInfoColumn.h"
#include "LPrefabEditorModule.h"
#include "ActorTreeItem.h"
#include "SortHelper.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Text/STextBlock.h"
#include "LPrefabUtils.h"
#include "LPrefabEditorStyle.h"
#include "SceneOutliner/LPrefabSceneOutlinerButton.h"
#include "LPrefabEditorTools.h"
#include "SortHelper.h"
#include "PrefabSystem/LPrefabHelperObject.h"
#include "PrefabSystem/LPrefabManager.h"
#include "PrefabEditor/LPrefabEditor.h"
#include "SceneOutlinerStandaloneTypes.h"

#define LOCTEXT_NAMESPACE "LPrefabSceneOutlinerInfoColumn"

namespace LPrefabSceneOutliner
{
	TSharedRef<ISceneOutlinerColumn> FLPrefabSceneOutlinerInfoColumn::MakeInstance(ISceneOutliner& SceneOutliner)
	{
		return MakeShareable(new FLPrefabSceneOutlinerInfoColumn(SceneOutliner));
	}


	FLPrefabSceneOutlinerInfoColumn::FLPrefabSceneOutlinerInfoColumn(ISceneOutliner& InSceneOutliner)
		: WeakSceneOutliner(StaticCastSharedRef<ISceneOutliner>(InSceneOutliner.AsShared()))
	{
		
	}

	FLPrefabSceneOutlinerInfoColumn::~FLPrefabSceneOutlinerInfoColumn()
	{
	}
	FName FLPrefabSceneOutlinerInfoColumn::GetID()
	{
		static FName LPrefabInfoID("LPrefab");
		return LPrefabInfoID;
	}

	FName FLPrefabSceneOutlinerInfoColumn::GetColumnID()
	{
		return GetID();
	}

	SHeaderRow::FColumn::FArguments FLPrefabSceneOutlinerInfoColumn::ConstructHeaderRowColumn()
	{
		return SHeaderRow::Column(GetColumnID())
			.DefaultLabel(LOCTEXT("LPrefabColumeHeader", "LPrefab"))
			.DefaultTooltip(LOCTEXT("LPrefabColumeHeader_Tooltip", "LPrefab functions"))
			.HAlignHeader(EHorizontalAlignment::HAlign_Center)
			;
	}

	const TSharedRef< SWidget > FLPrefabSceneOutlinerInfoColumn::ConstructRowWidget(FSceneOutlinerTreeItemRef TreeItem, const STableRow<FSceneOutlinerTreeItemPtr>& Row)
	{
		auto SceneOutliner = WeakSceneOutliner.Pin();
		check(SceneOutliner.IsValid());

		AActor* actor = GetActorFromTreeItem(TreeItem);
		if (actor == nullptr)
		{
			return SNew(SBox);
		}
		if (!LPrefabEditorTools::IsActorCompatibleWithLGUIToolsMenu(actor))
		{
			return SNew(SBox);
		}

		auto bIsRootAgentActor = FLPrefabEditor::ActorIsRootAgent(actor);
		TSharedRef<SLPrefabSceneOutlinerButton> result = SNew(SLPrefabSceneOutlinerButton)
			.ButtonStyle(FLPrefabEditorStyle::Get(), "EmptyButton")
			.ContentPadding(FMargin(0))
			.HasDownArrow(false)
			.OnComboBoxOpened(FOnComboBoxOpened::CreateLambda([=]() {//@todo: make it a callback
				FLPrefabEditorModule::Get().OnOutlinerSelectionChange();
				}))
			.Visibility(bIsRootAgentActor ? EVisibility::HitTestInvisible : EVisibility::Visible)
			.ButtonContent()
			[
				SNew(SHorizontalBox)
				+SHorizontalBox::Slot()
				[
					SNew(SOverlay)
					+SOverlay::Slot()//canvas icon
					[
						SNew(SBox)
						.WidthOverride(16)
						.HeightOverride(16)
						.Padding(FMargin(0))
						.HAlign(EHorizontalAlignment::HAlign_Center)
						.VAlign(EVerticalAlignment::VAlign_Center)
						[
							SNew(SImage)
							.Image(FLPrefabEditorStyle::Get().GetBrush("CanvasMark"))
							.Visibility(this, &FLPrefabSceneOutlinerInfoColumn::GetCanvasIconVisibility, TreeItem)
							.ColorAndOpacity(this, &FLPrefabSceneOutlinerInfoColumn::GetDrawcallIconColor, TreeItem)
							.ToolTipText(LOCTEXT("CanvasMarkTip", "This actor have LGUICanvas. The number is the drawcall count of this canvas."))
						]
					]
					+SOverlay::Slot()//drawcall count
					[
						SNew(SBox)
						.WidthOverride(16)
						.HeightOverride(16)
						.Padding(FMargin(0))
						.HAlign(EHorizontalAlignment::HAlign_Left)
						.VAlign(EVerticalAlignment::VAlign_Center)
						[
							SNew(STextBlock)
							.ShadowColorAndOpacity(FLinearColor::Black)
							.ShadowOffset(FVector2D(1, 1))
							.Text(this, &FLPrefabSceneOutlinerInfoColumn::GetDrawcallInfo, TreeItem)
							.ColorAndOpacity(FSlateColor(FLinearColor(FColor::Green)))
							.Visibility(this, &FLPrefabSceneOutlinerInfoColumn::GetDrawcallCountVisibility, TreeItem)
							.ToolTipText(LOCTEXT("DrawcallCountTip", "The number is the drawcall count generated by this LGUICanvas."))
							.Font(IDetailLayoutBuilder::GetDetailFont())
						]
					]
				]
				+SHorizontalBox::Slot()
				[
					SNew(SOverlay)
					+SOverlay::Slot()//down arrow
					[
						SNew(SBox)
						.Visibility(bIsRootAgentActor ? EVisibility::Hidden : EVisibility::Visible)
						.WidthOverride(8)
						.HeightOverride(8)
						.Padding(FMargin(0))
						.HAlign(EHorizontalAlignment::HAlign_Center)
						.VAlign(EVerticalAlignment::VAlign_Center)
						[
							SNew(SImage)
							.Visibility(this, &FLPrefabSceneOutlinerInfoColumn::GetDownArrowVisibility, TreeItem)
							.Image(FAppStyle::GetBrush("ComboButton.Arrow"))
							.ColorAndOpacity(FSlateColor::UseForeground())
						]
					]
					+SOverlay::Slot()//prefab
					[
						SNew(SBox)
						.WidthOverride(16)
						.HeightOverride(16)
						.Padding(FMargin(0))
						.HAlign(EHorizontalAlignment::HAlign_Center)
						.VAlign(EVerticalAlignment::VAlign_Center)
						[
							SNew(SImage)
							.Image(this, &FLPrefabSceneOutlinerInfoColumn::GetPrefabIconImage, TreeItem)
							.ColorAndOpacity(this, &FLPrefabSceneOutlinerInfoColumn::GetPrefabIconColor, TreeItem)
							.Visibility(this, &FLPrefabSceneOutlinerInfoColumn::GetPrefabIconVisibility, TreeItem)
							.ToolTipText(this, &FLPrefabSceneOutlinerInfoColumn::GetPrefabTooltip, TreeItem)
						]
					]
					//+SOverlay::Slot()//prefab+
					//[
					//	SNew(SBox)
					//	.WidthOverride(16)
					//	.HeightOverride(16)
					//	.Padding(FMargin(0))
					//	.HAlign(EHorizontalAlignment::HAlign_Center)
					//	.VAlign(EVerticalAlignment::VAlign_Center)
					//	[
					//		SNew(SImage)
					//		.Image(this, &FLPrefabSceneOutlinerInfoColumn::GetPrefabPlusIconImage, TreeItem)
					//		.Visibility(this, &FLPrefabSceneOutlinerInfoColumn::GetPrefabPlusIconVisibility, TreeItem)
					//	]
					//]
				]
			]
			.MenuContent()
			[
				FLPrefabEditorModule::Get().MakeEditorToolsMenu(false, false, false, false, false)
			];

		result->_TreeItemActor = actor;

		return result;
	}

	void FLPrefabSceneOutlinerInfoColumn::PopulateSearchStrings(const ISceneOutlinerTreeItem& Item, TArray< FString >& OutSearchStrings) const
	{
		OutSearchStrings.Add(Item.GetDisplayString());
	}

	void FLPrefabSceneOutlinerInfoColumn::SortItems(TArray<FSceneOutlinerTreeItemPtr>& OutItems, const EColumnSortMode::Type SortMode) const
	{
		if (SortMode == EColumnSortMode::None)return;

		OutItems.Sort([this, SortMode](FSceneOutlinerTreeItemPtr A, FSceneOutlinerTreeItemPtr B)
		{
			auto CommonCompare = [&] {
				auto AStr = SceneOutliner::FNumericStringWrapper(A->GetDisplayString());
				auto BStr = SceneOutliner::FNumericStringWrapper(B->GetDisplayString());
				return AStr > BStr;
				};

			AActor* ActorA = GetActorFromTreeItem(A.ToSharedRef());
			AActor* ActorB = GetActorFromTreeItem(B.ToSharedRef());

			bool result = false;
			if (FLPrefabEditorModule::PrefabEditor_SortActorOnLGUIInfoColumn.IsBound())
			{
				result = FLPrefabEditorModule::PrefabEditor_SortActorOnLGUIInfoColumn.Execute(ActorA, ActorB, CommonCompare);
			}
			else
			{
				result = CommonCompare();
			}
			return SortMode == EColumnSortMode::Ascending ? !result : result;
		});
	}

	EVisibility FLPrefabSceneOutlinerInfoColumn::GetPrefabIconVisibility(FSceneOutlinerTreeItemRef TreeItem)const
	{
		if (AActor* actor = GetActorFromTreeItem(TreeItem))
		{
			if (auto PrefabHelperObject = LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(actor))
			{
				if (PrefabHelperObject->IsActorBelongsToSubPrefab(actor))//is sub prefab
				{
					return EVisibility::Visible;
				}
				else
				{
					if (PrefabHelperObject->IsActorBelongsToMissingSubPrefab(actor))
					{
						return EVisibility::Visible;
					}
					else
					{
						return EVisibility::Hidden;
					}
				}
			}
		}
		return EVisibility::Hidden;
	}
	EVisibility FLPrefabSceneOutlinerInfoColumn::GetDownArrowVisibility(FSceneOutlinerTreeItemRef TreeItem)const
	{
		if (AActor* actor = GetActorFromTreeItem(TreeItem))
		{
			return GetPrefabIconVisibility(TreeItem) == EVisibility::Visible ? EVisibility::Hidden : EVisibility::Visible;
		}
		else
		{
			return EVisibility::Hidden;
		}
	}
	EVisibility FLPrefabSceneOutlinerInfoColumn::GetCanvasIconVisibility(FSceneOutlinerTreeItemRef TreeItem)const
	{
		if (AActor* actor = GetActorFromTreeItem(TreeItem))
		{
			if (FLPrefabEditorModule::PrefabEditor_IsCanvasActor.IsBound())
			{
				return FLPrefabEditorModule::PrefabEditor_IsCanvasActor.Execute(actor) ? EVisibility::Visible : EVisibility::Hidden;
			}
			else
			{
				return EVisibility::Hidden;
			}
		}
		return EVisibility::Hidden;
	}
	EVisibility FLPrefabSceneOutlinerInfoColumn::GetDrawcallCountVisibility(FSceneOutlinerTreeItemRef TreeItem)const
	{
		if (AActor* actor = GetActorFromTreeItem(TreeItem))
		{
			if (FLPrefabEditorModule::PrefabEditor_IsCanvasActor.IsBound())
			{
				return FLPrefabEditorModule::PrefabEditor_IsCanvasActor.Execute(actor) ? EVisibility::Visible : EVisibility::Hidden;
			}
			else
			{
				return EVisibility::Hidden;
			}
		}
		return EVisibility::Hidden;
	}
	auto ActorIsPrefabPlus(AActor* Actor)
	{
		if (auto ParentActor = Actor->GetAttachParentActor())
		{
			auto PrefabHelperObjectForParent = LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(ParentActor);
			auto PrefabHelperObject = LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(Actor);
			if (PrefabHelperObject != nullptr && PrefabHelperObjectForParent != nullptr)
			{
				if (!PrefabHelperObject->IsActorBelongsToSubPrefab(Actor) && PrefabHelperObjectForParent->IsActorBelongsToSubPrefab(ParentActor))
				{
					return true;
				}
			}
		}
		return false;
	}
	EVisibility FLPrefabSceneOutlinerInfoColumn::GetPrefabPlusIconVisibility(FSceneOutlinerTreeItemRef TreeItem)const
	{
		if (AActor* Actor = GetActorFromTreeItem(TreeItem))
		{
			if (ActorIsPrefabPlus(Actor))
				return EVisibility::Visible;
		}
		return EVisibility::Hidden;
	}
	FSlateColor FLPrefabSceneOutlinerInfoColumn::GetDrawcallIconColor(FSceneOutlinerTreeItemRef TreeItem)const
	{
		if (AActor* actor = GetActorFromTreeItem(TreeItem))
		{
			if (FLPrefabEditorModule::PrefabEditor_IsCanvasActor.IsBound())
			{
				return FLPrefabEditorModule::PrefabEditor_IsCanvasActor.Execute(actor) ? FSlateColor(FLinearColor(1.0f, 1.0f, 1.0f, 0.4f)) : FSlateColor(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f));
			}
			else
			{
				return FSlateColor(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f));
			}
		}
		return FSlateColor(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f));
	}
	FText FLPrefabSceneOutlinerInfoColumn::GetDrawcallInfo(FSceneOutlinerTreeItemRef TreeItem)const
	{
		int drawcallCount = 0;
		if (AActor* actor = GetActorFromTreeItem(TreeItem))
		{
			if (FLPrefabEditorModule::PrefabEditor_GetCanvasActorDrawcallCount.IsBound())
			{
				drawcallCount = FLPrefabEditorModule::PrefabEditor_GetCanvasActorDrawcallCount.Execute(actor);
			}
			else
			{
				drawcallCount = 0;
			}
		}
		return FText::FromString(FString::Printf(TEXT("%d"), drawcallCount));
	}
	const FSlateBrush* FLPrefabSceneOutlinerInfoColumn::GetPrefabIconImage(FSceneOutlinerTreeItemRef TreeItem)const
	{
		if (auto actor = GetActorFromTreeItem(TreeItem))
		{
			if (auto PrefabHelperObject = LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(actor))
			{
				if (!PrefabHelperObject->IsActorBelongsToSubPrefab(actor))//is sub prefab
				{
					if (PrefabHelperObject->IsActorBelongsToMissingSubPrefab(actor))
					{
						return FLPrefabEditorStyle::Get().GetBrush("PrefabMarkBroken");
					}
				}
				else
				{
					if (PrefabHelperObject->GetSubPrefabAsset(actor)->GetIsPrefabVariant())
					{
						return FLPrefabEditorStyle::Get().GetBrush("PrefabVariantMarkWhite");
					}
				}
			}
		}
		return FLPrefabEditorStyle::Get().GetBrush("PrefabMarkWhite");
	}
	const FSlateBrush* FLPrefabSceneOutlinerInfoColumn::GetPrefabPlusIconImage(FSceneOutlinerTreeItemRef TreeItem)const
	{
		return FLPrefabEditorStyle::Get().GetBrush("PrefabPlusMarkWhite");
	}
	FSlateColor FLPrefabSceneOutlinerInfoColumn::GetPrefabIconColor(FSceneOutlinerTreeItemRef TreeItem)const
	{
		if (auto actor = GetActorFromTreeItem(TreeItem))
		{
			if (auto PrefabHelperObject = LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(actor))
			{
				if (PrefabHelperObject->IsActorBelongsToSubPrefab(actor))//is sub prefab
				{
					return FSlateColor(PrefabHelperObject->GetSubPrefabData(actor).EditorIdentifyColor);
				}
				else
				{
					if (PrefabHelperObject->IsActorBelongsToMissingSubPrefab(actor))
					{
						return FSlateColor(FColor::White);
					}
				}
			}
		}
		return FSlateColor(FColor::Green);
	}
	FText FLPrefabSceneOutlinerInfoColumn::GetPrefabTooltip(FSceneOutlinerTreeItemRef TreeItem)const
	{
		if (auto actor = GetActorFromTreeItem(TreeItem))
		{
			if (auto PrefabHelperObject = LPrefabEditorTools::GetPrefabHelperObject_WhichManageThisActor(actor))
			{
				if (!PrefabHelperObject->IsActorBelongsToSubPrefab(actor))//is sub prefab
				{
					if (PrefabHelperObject->IsActorBelongsToMissingSubPrefab(actor))
					{
						return LOCTEXT("PrefabMarkBrokenTip", "This actor was part of a LPrefab, but the prefab asset is missing!");
					}
				}
			}
		}
		return LOCTEXT("PrefabMarkWhiteTip", "This actor is part of a LPrefab.");
	}

	AActor* FLPrefabSceneOutlinerInfoColumn::GetActorFromTreeItem(FSceneOutlinerTreeItemRef TreeItem)const
	{
		if (auto ActorTreeItem = TreeItem->CastTo<FActorTreeItem>())
		{
			if (ActorTreeItem->Actor.IsValid() && !ActorTreeItem->Actor->IsPendingKillPending())
			{
				if (ActorTreeItem->Actor->GetWorld())
				{
					return ActorTreeItem->Actor.Get();
				}
			}
		}
		return nullptr;
	}
}

#undef LOCTEXT_NAMESPACE