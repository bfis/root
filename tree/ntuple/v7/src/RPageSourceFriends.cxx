/// \file RPageSourceFriends.cxx
/// \ingroup NTuple ROOT7
/// \author Jakob Blomer <jblomer@cern.ch>
/// \date 2019-08-10
/// \warning This is part of the ROOT 7 prototype! It will change without notice. It might trigger earthquakes. Feedback
/// is welcome!

/*************************************************************************
 * Copyright (C) 1995-2020, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include <ROOT/RCluster.hxx>
#include <ROOT/RError.hxx>
#include <ROOT/RLogger.hxx>
#include <ROOT/RNTupleReadOptions.hxx>
#include <ROOT/RPageSourceFriends.hxx>

#include <utility>

ROOT::Experimental::Internal::RPageSourceFriends::RPageSourceFriends(std::string_view ntupleName,
                                                                     std::span<std::unique_ptr<RPageSource>> sources)
   : RPageSource(ntupleName, RNTupleReadOptions()), fMetrics(std::string(ntupleName))
{
   for (auto &s : sources) {
      fSources.emplace_back(std::move(s));
      fMetrics.ObserveMetrics(fSources.back()->GetMetrics());
   }
}

ROOT::Experimental::Internal::RPageSourceFriends::~RPageSourceFriends() = default;

void ROOT::Experimental::Internal::RPageSourceFriends::AddVirtualField(const RNTupleDescriptor &originDesc,
                                                                       std::size_t originIdx,
                                                                       const RFieldDescriptor &originField,
                                                                       DescriptorId_t virtualParent,
                                                                       const std::string &virtualName)
{
   auto virtualFieldId = fNextId++;
   auto virtualField =
      RFieldDescriptorBuilder(originField).FieldId(virtualFieldId).FieldName(virtualName).MakeDescriptor().Unwrap();
   fBuilder.AddField(virtualField);
   fBuilder.AddFieldLink(virtualParent, virtualFieldId);
   fIdBiMap.Insert({originIdx, originField.GetId()}, virtualFieldId);

   for (const auto &f : originDesc.GetFieldIterable(originField))
      AddVirtualField(originDesc, originIdx, f, virtualFieldId, f.GetFieldName());

   for (const auto &c : originDesc.GetColumnIterable(originField)) {
      auto physicalId = c.IsAliasColumn() ? fIdBiMap.GetVirtualId({originIdx, c.GetPhysicalId()}) : fNextId;
      RColumnDescriptorBuilder columnBuilder;
      columnBuilder.LogicalColumnId(fNextId)
         .PhysicalColumnId(physicalId)
         .FieldId(virtualFieldId)
         .BitsOnStorage(c.GetBitsOnStorage())
         .Type(c.GetType())
         .Index(c.GetIndex())
         .RepresentationIndex(c.GetRepresentationIndex());
      fBuilder.AddColumn(columnBuilder.MakeDescriptor().Unwrap()).ThrowOnError();
      fIdBiMap.Insert({originIdx, c.GetLogicalId()}, fNextId);
      fNextId++;
   }
}

ROOT::Experimental::RNTupleDescriptor ROOT::Experimental::Internal::RPageSourceFriends::AttachImpl()
{
   fBuilder.SetNTuple(fNTupleName, "");
   fBuilder.AddField(
      RFieldDescriptorBuilder().FieldId(0).Structure(ENTupleStructure::kRecord).MakeDescriptor().Unwrap());

   for (std::size_t i = 0; i < fSources.size(); ++i) {
      fSources[i]->Attach();

      if (fSources[i]->GetNEntries() != fSources[0]->GetNEntries()) {
         fNextId = 1;
         fIdBiMap.Clear();
         fBuilder.Reset();
         throw RException(R__FAIL("mismatch in the number of entries of friend RNTuples"));
      }

      auto descriptorGuard = fSources[i]->GetSharedDescriptorGuard();
      for (unsigned j = 0; j < i; ++j) {
         if (fSources[j]->GetSharedDescriptorGuard()->GetName() == descriptorGuard->GetName()) {
            fNextId = 1;
            fIdBiMap.Clear();
            fBuilder.Reset();
            throw RException(R__FAIL("duplicate names of friend RNTuples"));
         }
      }
      AddVirtualField(descriptorGuard.GetRef(), i, descriptorGuard->GetFieldZero(), 0, descriptorGuard->GetName());

      for (const auto &cg : descriptorGuard->GetClusterGroupIterable()) {
         auto clusterGroupBuilder = Internal::RClusterGroupDescriptorBuilder::FromSummary(cg);
         clusterGroupBuilder.ClusterGroupId(fNextId);
         fBuilder.AddClusterGroup(clusterGroupBuilder.MoveDescriptor().Unwrap());
         fIdBiMap.Insert({i, cg.GetId()}, fNextId);
         fNextId++;
      }

      for (const auto &c : descriptorGuard->GetClusterIterable()) {
         RClusterDescriptorBuilder clusterBuilder;
         clusterBuilder.ClusterId(fNextId).FirstEntryIndex(c.GetFirstEntryIndex()).NEntries(c.GetNEntries());
         for (const auto &originColumnRange : c.GetColumnRangeIterable()) {
            DescriptorId_t virtualColumnId = fIdBiMap.GetVirtualId({i, originColumnRange.fPhysicalColumnId});

            auto pageRange = c.GetPageRange(originColumnRange.fPhysicalColumnId).Clone();
            pageRange.fPhysicalColumnId = virtualColumnId;

            auto firstElementIndex = originColumnRange.fFirstElementIndex;
            auto compressionSettings = originColumnRange.fCompressionSettings;

            clusterBuilder.CommitColumnRange(virtualColumnId, firstElementIndex, compressionSettings, pageRange);
         }
         fBuilder.AddCluster(clusterBuilder.MoveDescriptor().Unwrap());
         fIdBiMap.Insert({i, c.GetId()}, fNextId);
         fNextId++;
      }
   }

   fBuilder.EnsureValidDescriptor();
   return fBuilder.MoveDescriptor();
}

std::unique_ptr<ROOT::Experimental::Internal::RPageSource>
ROOT::Experimental::Internal::RPageSourceFriends::CloneImpl() const
{
   std::vector<std::unique_ptr<RPageSource>> cloneSources;
   cloneSources.reserve(fSources.size());
   for (const auto &f : fSources)
      cloneSources.emplace_back(f->Clone());
   auto clone = std::make_unique<RPageSourceFriends>(fNTupleName, cloneSources);
   clone->fIdBiMap = fIdBiMap;
   return clone;
}

ROOT::Experimental::Internal::RPageStorage::ColumnHandle_t
ROOT::Experimental::Internal::RPageSourceFriends::AddColumn(DescriptorId_t fieldId, const RColumn &column)
{
   auto originFieldId = fIdBiMap.GetOriginId(fieldId);
   fSources[originFieldId.fSourceIdx]->AddColumn(originFieldId.fId, column);
   return RPageSource::AddColumn(fieldId, column);
}

void ROOT::Experimental::Internal::RPageSourceFriends::DropColumn(ColumnHandle_t columnHandle)
{
   RPageSource::DropColumn(columnHandle);
   auto originColumnId = fIdBiMap.GetOriginId(columnHandle.fPhysicalId);
   columnHandle.fPhysicalId = originColumnId.fId;
   fSources[originColumnId.fSourceIdx]->DropColumn(columnHandle);
}

ROOT::Experimental::Internal::RPage
ROOT::Experimental::Internal::RPageSourceFriends::PopulatePage(ColumnHandle_t columnHandle, NTupleSize_t globalIndex)
{
   auto virtualColumnId = columnHandle.fPhysicalId;
   auto originColumnId = fIdBiMap.GetOriginId(virtualColumnId);
   columnHandle.fPhysicalId = originColumnId.fId;

   auto page = fSources[originColumnId.fSourceIdx]->PopulatePage(columnHandle, globalIndex);

   auto virtualClusterId = fIdBiMap.GetVirtualId({originColumnId.fSourceIdx, page.GetClusterInfo().GetId()});
   page.ChangeIds(virtualColumnId, virtualClusterId);

   return page;
}

ROOT::Experimental::Internal::RPage
ROOT::Experimental::Internal::RPageSourceFriends::PopulatePage(ColumnHandle_t columnHandle, RClusterIndex clusterIndex)
{
   auto virtualColumnId = columnHandle.fPhysicalId;
   auto originColumnId = fIdBiMap.GetOriginId(virtualColumnId);
   RClusterIndex originClusterIndex(fIdBiMap.GetOriginId(clusterIndex.GetClusterId()).fId, clusterIndex.GetIndex());
   columnHandle.fPhysicalId = originColumnId.fId;

   auto page = fSources[originColumnId.fSourceIdx]->PopulatePage(columnHandle, originClusterIndex);

   page.ChangeIds(virtualColumnId, clusterIndex.GetClusterId());
   return page;
}

void ROOT::Experimental::Internal::RPageSourceFriends::LoadSealedPage(DescriptorId_t physicalColumnId,
                                                                      RClusterIndex clusterIndex,
                                                                      RSealedPage &sealedPage)
{
   auto originColumnId = fIdBiMap.GetOriginId(physicalColumnId);
   RClusterIndex originClusterIndex(fIdBiMap.GetOriginId(clusterIndex.GetClusterId()).fId, clusterIndex.GetIndex());

   fSources[originColumnId.fSourceIdx]->LoadSealedPage(physicalColumnId, originClusterIndex, sealedPage);
}

void ROOT::Experimental::Internal::RPageSourceFriends::ReleasePage(RPage &page)
{
   if (page.IsNull())
      return;
   auto sourceIdx = fIdBiMap.GetOriginId(page.GetClusterInfo().GetId()).fSourceIdx;
   fSources[sourceIdx]->ReleasePage(page);
}

std::vector<std::unique_ptr<ROOT::Experimental::Internal::RCluster>>
ROOT::Experimental::Internal::RPageSourceFriends::LoadClusters(std::span<RCluster::RKey> clusterKeys)
{
   // The virtual friends page source does not pre-load any clusters itself. However, the underlying page sources
   // that are combined may well do it.
   return std::vector<std::unique_ptr<ROOT::Experimental::Internal::RCluster>>(clusterKeys.size());
}
