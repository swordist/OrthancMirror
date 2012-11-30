/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012 Medical Physics Department, CHU of Liege,
 * Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * In addition, as a special exception, the copyright holders of this
 * program give permission to link the code of its release with the
 * OpenSSL project's "OpenSSL" library (or with modified versions of it
 * that use the same license as the "OpenSSL" library), and distribute
 * the linked executables. You must obey the GNU General Public License
 * in all respects for all of the code used other than "OpenSSL". If you
 * modify file(s) with this exception, you may extend this exception to
 * your version of the file(s), but you are not obligated to do so. If
 * you do not wish to do so, delete this exception statement from your
 * version. If you delete this exception statement from all source files
 * in the program, then also delete it here.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 **/


#include "ServerIndex.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "EmbeddedResources.h"
#include "../Core/Toolbox.h"
#include "../Core/Uuid.h"
#include "../Core/DicomFormat/DicomArray.h"
#include "../Core/SQLite/Transaction.h"
#include "FromDcmtkBridge.h"
#include "ServerContext.h"

#include <boost/lexical_cast.hpp>
#include <stdio.h>
#include <glog/logging.h>

namespace Orthanc
{
  namespace Internals
  {
    class ServerIndexListener : public IServerIndexListener
    {
    private:
      ServerContext& context_;
      bool hasRemainingLevel_;
      ResourceType remainingType_;
      std::string remainingPublicId_;

    public:
      ServerIndexListener(ServerContext& context) : 
        context_(context),
        hasRemainingLevel_(false)
      {
        assert(ResourceType_Patient < ResourceType_Study &&
               ResourceType_Study < ResourceType_Series &&
               ResourceType_Series < ResourceType_Instance);
      }

      void Reset()
      {
        hasRemainingLevel_ = false;
      }

      virtual void SignalRemainingAncestor(ResourceType parentType,
                                           const std::string& publicId)
      {
        LOG(INFO) << "Remaining ancestor \"" << publicId << "\" (" << parentType << ")";

        if (hasRemainingLevel_)
        {
          if (parentType < remainingType_)
          {
            remainingType_ = parentType;
            remainingPublicId_ = publicId;
          }
        }
        else
        {
          hasRemainingLevel_ = true;
          remainingType_ = parentType;
          remainingPublicId_ = publicId;
        }        
      }

      virtual void SignalFileDeleted(const std::string& fileUuid)
      {
        assert(Toolbox::IsUuid(fileUuid));
        context_.RemoveFile(fileUuid);
      }

      bool HasRemainingLevel() const
      {
        return hasRemainingLevel_;
      }

      ResourceType GetRemainingType() const
      {
        assert(HasRemainingLevel());
        return remainingType_;
      }

      const std::string& GetRemainingPublicId() const
      {
        assert(HasRemainingLevel());
        return remainingPublicId_;
      }                                 
    };
  }


  bool ServerIndex::DeleteResource(Json::Value& target,
                                   const std::string& uuid,
                                   ResourceType expectedType)
  {
    boost::mutex::scoped_lock lock(mutex_);

    listener_->Reset();

    std::auto_ptr<SQLite::Transaction> t(db_->StartTransaction());
    t->Begin();

    int64_t id;
    ResourceType type;
    if (!db_->LookupResource(uuid, id, type) ||
        expectedType != type)
    {
      return false;
    }
      
    db_->DeleteResource(id);

    if (listener_->HasRemainingLevel())
    {
      ResourceType type = listener_->GetRemainingType();
      const std::string& uuid = listener_->GetRemainingPublicId();

      target["RemainingAncestor"] = Json::Value(Json::objectValue);
      target["RemainingAncestor"]["Path"] = GetBasePath(type, uuid);
      target["RemainingAncestor"]["Type"] = ToString(type);
      target["RemainingAncestor"]["ID"] = uuid;
    }
    else
    {
      target["RemainingAncestor"] = Json::nullValue;
    }

    t->Commit();

    return true;
  }


  static void FlushThread(DatabaseWrapper* db,
                          boost::mutex* mutex,
                          unsigned int sleep)
  {
    LOG(INFO) << "Starting the database flushing thread (sleep = " << sleep << ")";

    while (1)
    {
      boost::this_thread::sleep(boost::posix_time::seconds(sleep));
      boost::mutex::scoped_lock lock(*mutex);
      db->FlushToDisk();
    }
  }


  ServerIndex::ServerIndex(ServerContext& context,
                           const std::string& dbPath) : mutex_()
  {
    listener_.reset(new Internals::ServerIndexListener(context));

    if (dbPath == ":memory:")
    {
      db_.reset(new DatabaseWrapper(*listener_));
    }
    else
    {
      boost::filesystem::path p = dbPath;

      try
      {
        boost::filesystem::create_directories(p);
      }
      catch (boost::filesystem::filesystem_error)
      {
      }

      db_.reset(new DatabaseWrapper(p.string() + "/index", *listener_));
    }

    unsigned int sleep;
    try
    {
      std::string sleepString = db_->GetGlobalProperty(GlobalProperty_FlushSleep);
      sleep = boost::lexical_cast<unsigned int>(sleepString);
    }
    catch (boost::bad_lexical_cast&)
    {
      // By default, wait for 10 seconds before flushing
      sleep = 10;
    }

    flushThread_ = boost::thread(FlushThread, db_.get(), &mutex_, sleep);
  }


  ServerIndex::~ServerIndex()
  {
    LOG(INFO) << "Stopping the database flushing thread";
    /*flushThread_.terminate();
      flushThread_.join();*/
  }


  StoreStatus ServerIndex::Store(const DicomMap& dicomSummary,
                                 const Attachments& attachments,
                                 const std::string& remoteAet)
  {
    boost::mutex::scoped_lock lock(mutex_);

    DicomInstanceHasher hasher(dicomSummary);

    try
    {
      std::auto_ptr<SQLite::Transaction> t(db_->StartTransaction());
      t->Begin();

      int64_t patient, study, series, instance;
      ResourceType type;
      bool isNewSeries = false;

      // Do nothing if the instance already exists
      if (db_->LookupResource(hasher.HashInstance(), patient, type))
      {
        assert(type == ResourceType_Instance);
        return StoreStatus_AlreadyStored;
      }

      // Create the instance
      instance = db_->CreateResource(hasher.HashInstance(), ResourceType_Instance);

      DicomMap dicom;
      dicomSummary.ExtractInstanceInformation(dicom);
      db_->SetMainDicomTags(instance, dicom);

      // Create the patient/study/series/instance hierarchy
      if (!db_->LookupResource(hasher.HashSeries(), series, type))
      {
        // This is a new series
        isNewSeries = true;
        series = db_->CreateResource(hasher.HashSeries(), ResourceType_Series);
        dicomSummary.ExtractSeriesInformation(dicom);
        db_->SetMainDicomTags(series, dicom);
        db_->AttachChild(series, instance);

        if (!db_->LookupResource(hasher.HashStudy(), study, type))
        {
          // This is a new study
          study = db_->CreateResource(hasher.HashStudy(), ResourceType_Study);
          dicomSummary.ExtractStudyInformation(dicom);
          db_->SetMainDicomTags(study, dicom);
          db_->AttachChild(study, series);

          if (!db_->LookupResource(hasher.HashPatient(), patient, type))
          {
            // This is a new patient
            patient = db_->CreateResource(hasher.HashPatient(), ResourceType_Patient);
            dicomSummary.ExtractPatientInformation(dicom);
            db_->SetMainDicomTags(patient, dicom);
            db_->AttachChild(patient, study);
          }
          else
          {
            assert(type == ResourceType_Patient);
            db_->AttachChild(patient, study);
          }
        }
        else
        {
          assert(type == ResourceType_Study);
          db_->AttachChild(study, series);
        }
      }
      else
      {
        assert(type == ResourceType_Series);
        db_->AttachChild(series, instance);
      }

      // Attach the files to the newly created instance
      for (Attachments::const_iterator it = attachments.begin();
           it != attachments.end(); it++)
      {
        db_->AddAttachment(instance, *it);
      }

      // Attach the metadata
      db_->SetMetadata(instance, MetadataType_Instance_ReceptionDate, Toolbox::GetNowIsoString());
      db_->SetMetadata(instance, MetadataType_Instance_RemoteAet, remoteAet);

      const DicomValue* value;
      if ((value = dicomSummary.TestAndGetValue(DICOM_TAG_INSTANCE_NUMBER)) != NULL ||
          (value = dicomSummary.TestAndGetValue(DICOM_TAG_IMAGE_INDEX)) != NULL)
      {
        db_->SetMetadata(instance, MetadataType_Instance_IndexInSeries, value->AsString());
      }

      if (isNewSeries)
      {
        if ((value = dicomSummary.TestAndGetValue(DICOM_TAG_NUMBER_OF_SLICES)) != NULL ||
            (value = dicomSummary.TestAndGetValue(DICOM_TAG_IMAGES_IN_ACQUISITION)) != NULL ||
            (value = dicomSummary.TestAndGetValue(DICOM_TAG_CARDIAC_NUMBER_OF_IMAGES)) != NULL)
        {
          db_->SetMetadata(series, MetadataType_Series_ExpectedNumberOfInstances, value->AsString());
        }
      }

      // Check whether the series of this new instance is now completed
      SeriesStatus seriesStatus = GetSeriesStatus(series);
      if (seriesStatus == SeriesStatus_Complete)
      {
        db_->LogChange(ChangeType_CompletedSeries, series, ResourceType_Series);
      }

      t->Commit();

      return StoreStatus_Success;
    }
    catch (OrthancException& e)
    {
      LOG(ERROR) << "EXCEPTION2 [" << e.What() << "]" << " " << db_->GetErrorMessage();  
    }

    return StoreStatus_Failure;
  }


  void ServerIndex::ComputeStatistics(Json::Value& target)
  {
    boost::mutex::scoped_lock lock(mutex_);
    target = Json::objectValue;

    uint64_t cs = db_->GetTotalCompressedSize();
    uint64_t us = db_->GetTotalUncompressedSize();
    target["TotalDiskSize"] = boost::lexical_cast<std::string>(cs);
    target["TotalUncompressedSize"] = boost::lexical_cast<std::string>(us);
    target["TotalDiskSizeMB"] = boost::lexical_cast<unsigned int>(cs / (1024llu * 1024llu));
    target["TotalUncompressedSizeMB"] = boost::lexical_cast<unsigned int>(us / (1024llu * 1024llu));

    target["CountPatients"] = static_cast<unsigned int>(db_->GetResourceCount(ResourceType_Patient));
    target["CountStudies"] = static_cast<unsigned int>(db_->GetResourceCount(ResourceType_Study));
    target["CountSeries"] = static_cast<unsigned int>(db_->GetResourceCount(ResourceType_Series));
    target["CountInstances"] = static_cast<unsigned int>(db_->GetResourceCount(ResourceType_Instance));
  }          



  SeriesStatus ServerIndex::GetSeriesStatus(int id)
  {
    // Get the expected number of instances in this series (from the metadata)
    std::string s = db_->GetMetadata(id, MetadataType_Series_ExpectedNumberOfInstances);

    size_t expected;
    try
    {
      expected = boost::lexical_cast<size_t>(s);
      if (expected < 0)
      {
        return SeriesStatus_Unknown;
      }
    }
    catch (boost::bad_lexical_cast&)
    {
      return SeriesStatus_Unknown;
    }

    // Loop over the instances of this series
    std::list<int64_t> children;
    db_->GetChildrenInternalId(children, id);

    std::set<size_t> instances;
    for (std::list<int64_t>::const_iterator 
           it = children.begin(); it != children.end(); it++)
    {
      // Get the index of this instance in the series
      s = db_->GetMetadata(*it, MetadataType_Instance_IndexInSeries);
      size_t index;
      try
      {
        index = boost::lexical_cast<size_t>(s);
      }
      catch (boost::bad_lexical_cast&)
      {
        return SeriesStatus_Unknown;
      }

      if (index <= 0 || index > expected)
      {
        // Out-of-range instance index
        return SeriesStatus_Inconsistent;
      }

      if (instances.find(index) != instances.end())
      {
        // Twice the same instance index
        return SeriesStatus_Inconsistent;
      }

      instances.insert(index);
    }

    if (instances.size() == expected)
    {
      return SeriesStatus_Complete;
    }
    else
    {
      return SeriesStatus_Missing;
    }
  }



  void ServerIndex::MainDicomTagsToJson(Json::Value& target,
                                        int64_t resourceId)
  {
    DicomMap tags;
    db_->GetMainDicomTags(tags, resourceId);
    target["MainDicomTags"] = Json::objectValue;
    FromDcmtkBridge::ToJson(target["MainDicomTags"], tags);
  }

  bool ServerIndex::LookupResource(Json::Value& result,
                                   const std::string& publicId,
                                   ResourceType expectedType)
  {
    result = Json::objectValue;

    boost::mutex::scoped_lock lock(mutex_);

    // Lookup for the requested resource
    int64_t id;
    ResourceType type;
    if (!db_->LookupResource(publicId, id, type) ||
        type != expectedType)
    {
      return false;
    }

    // Find the parent resource (if it exists)
    if (type != ResourceType_Patient)
    {
      int64_t parentId;
      if (!db_->LookupParent(parentId, id))
      {
        throw OrthancException(ErrorCode_InternalError);
      }

      std::string parent = db_->GetPublicId(parentId);

      switch (type)
      {
      case ResourceType_Study:
        result["ParentPatient"] = parent;
        break;

      case ResourceType_Series:
        result["ParentStudy"] = parent;
        break;

      case ResourceType_Instance:
        result["ParentSeries"] = parent;
        break;

      default:
        throw OrthancException(ErrorCode_InternalError);
      }
    }

    // List the children resources
    std::list<std::string> children;
    db_->GetChildrenPublicId(children, id);

    if (type != ResourceType_Instance)
    {
      Json::Value c = Json::arrayValue;

      for (std::list<std::string>::const_iterator
             it = children.begin(); it != children.end(); it++)
      {
        c.append(*it);
      }

      switch (type)
      {
      case ResourceType_Patient:
        result["Studies"] = c;
        break;

      case ResourceType_Study:
        result["Series"] = c;
        break;

      case ResourceType_Series:
        result["Instances"] = c;
        break;

      default:
        throw OrthancException(ErrorCode_InternalError);
      }
    }

    // Set the resource type
    switch (type)
    {
    case ResourceType_Patient:
      result["Type"] = "Patient";
      break;

    case ResourceType_Study:
      result["Type"] = "Study";
      break;

    case ResourceType_Series:
    {
      result["Type"] = "Series";
      result["Status"] = ToString(GetSeriesStatus(id));

      int i;
      if (db_->GetMetadataAsInteger(i, id, MetadataType_Series_ExpectedNumberOfInstances))
        result["ExpectedNumberOfInstances"] = i;
      else
        result["ExpectedNumberOfInstances"] = Json::nullValue;

      break;
    }

    case ResourceType_Instance:
    {
      result["Type"] = "Instance";

      FileInfo attachment;
      if (!db_->LookupAttachment(attachment, id, FileContentType_Dicom))
      {
        throw OrthancException(ErrorCode_InternalError);
      }

      result["FileSize"] = static_cast<unsigned int>(attachment.GetUncompressedSize());
      result["FileUuid"] = attachment.GetUuid();

      int i;
      if (db_->GetMetadataAsInteger(i, id, MetadataType_Instance_IndexInSeries))
        result["IndexInSeries"] = i;
      else
        result["IndexInSeries"] = Json::nullValue;

      break;
    }

    default:
      throw OrthancException(ErrorCode_InternalError);
    }

    // Record the remaining information
    result["ID"] = publicId;
    MainDicomTagsToJson(result, id);

    return true;
  }


  bool ServerIndex::LookupAttachment(FileInfo& attachment,
                                     const std::string& instanceUuid,
                                     FileContentType contentType)
  {
    boost::mutex::scoped_lock lock(mutex_);

    int64_t id;
    ResourceType type;
    if (!db_->LookupResource(instanceUuid, id, type) ||
        type != ResourceType_Instance)
    {
      throw OrthancException(ErrorCode_InternalError);
    }

    if (db_->LookupAttachment(attachment, id, contentType))
    {
      assert(attachment.GetContentType() == contentType);
      return true;
    }
    else
    {
      return false;
    }
  }



  void ServerIndex::GetAllUuids(Json::Value& target,
                                ResourceType resourceType)
  {
    boost::mutex::scoped_lock lock(mutex_);
    db_->GetAllPublicIds(target, resourceType);
  }


  bool ServerIndex::GetChanges(Json::Value& target,
                               int64_t since,                               
                               unsigned int maxResults)
  {
    boost::mutex::scoped_lock lock(mutex_);
    db_->GetChanges(target, since, maxResults);
    return true;
  }

  bool ServerIndex::GetLastChange(Json::Value& target)
  {
    boost::mutex::scoped_lock lock(mutex_);
    db_->GetLastChange(target);
    return true;
  }

  void ServerIndex::LogExportedResource(const std::string& publicId,
                                        const std::string& remoteModality)
  {
    boost::mutex::scoped_lock lock(mutex_);

    int64_t id;
    ResourceType type;
    if (!db_->LookupResource(publicId, id, type))
    {
      throw OrthancException(ErrorCode_InternalError);
    }

    std::string patientId;
    std::string studyInstanceUid;
    std::string seriesInstanceUid;
    std::string sopInstanceUid;

    int64_t currentId = id;
    ResourceType currentType = type;

    // Iteratively go up inside the patient/study/series/instance hierarchy
    bool done = false;
    while (!done)
    {
      DicomMap map;
      db_->GetMainDicomTags(map, currentId);

      switch (currentType)
      {
      case ResourceType_Patient:
        patientId = map.GetValue(DICOM_TAG_PATIENT_ID).AsString();
        done = true;
        break;

      case ResourceType_Study:
        studyInstanceUid = map.GetValue(DICOM_TAG_STUDY_INSTANCE_UID).AsString();
        currentType = ResourceType_Patient;
        break;

      case ResourceType_Series:
        seriesInstanceUid = map.GetValue(DICOM_TAG_SERIES_INSTANCE_UID).AsString();
        currentType = ResourceType_Study;
        break;

      case ResourceType_Instance:
        sopInstanceUid = map.GetValue(DICOM_TAG_SOP_INSTANCE_UID).AsString();
        currentType = ResourceType_Series;
        break;

      default:
        throw OrthancException(ErrorCode_InternalError);
      }

      // If we have not reached the Patient level, find the parent of
      // the current resource
      if (!done)
      {
        assert(db_->LookupParent(currentId, currentId));
      }
    }

    // No need for a SQLite::Transaction here, as we only insert 1 record
    db_->LogExportedResource(type,
                             publicId,
                             remoteModality,
                             patientId,
                             studyInstanceUid,
                             seriesInstanceUid,
                             sopInstanceUid);
  }


  bool ServerIndex::GetExportedResources(Json::Value& target,
                                         int64_t since,
                                         unsigned int maxResults)
  {
    boost::mutex::scoped_lock lock(mutex_);
    db_->GetExportedResources(target, since, maxResults);
    return true;
  }

  bool ServerIndex::GetLastExportedResource(Json::Value& target)
  {
    boost::mutex::scoped_lock lock(mutex_);
    db_->GetLastExportedResource(target);
    return true;
  }
}
