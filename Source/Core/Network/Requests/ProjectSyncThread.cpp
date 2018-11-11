/*
    This file is part of Helio Workstation.

    Helio is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Helio is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Helio. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Common.h"
#include "ProjectSyncThread.h"
#include "HelioApiRoutes.h"
#include "SerializationKeys.h"
#include "RevisionDto.h"
#include "ProjectDto.h"
#include "ProjectSyncHelpers.h"

#include "App.h"
#include "Workspace.h"

namespace ApiKeys = Serialization::Api::V1;
namespace ApiRoutes = Routes::HelioFM::Api;

ProjectSyncThread::ProjectSyncThread() : Thread("Sync") {}

ProjectSyncThread::~ProjectSyncThread()
{
    this->stopThread(1000);
}

void ProjectSyncThread::doSync(WeakReference<VersionControl> vcs,
    const String &projectId, const String &projectName,
    const Array<String> &revisionIdsToSync)
{
    if (this->isThreadRunning())
    {
        Logger::writeToLog("Warning: failed to start revision sync thread, already running");
        return;
    }

    this->vcs = vcs;
    this->projectId = projectId;
    this->projectName = projectName;
    this->idsToSync = revisionIdsToSync;
    this->startThread(7);
}

void ProjectSyncThread::run()
{
    RevisionsMap localRevisions;
    ProjectSyncHelpers::buildLocalRevisionsIndex(localRevisions, this->vcs->getRoot());

    const String projectRoute(ApiRoutes::project.replace(":projectId", this->projectId));
    const BackendRequest revisionsRequest(projectRoute);
    this->response = revisionsRequest.get();

    const ProjectDto remoteProject(this->response.getBody());

    if (this->response.is(404))
    {
        // Put the project:
        const BackendRequest createProjectRequest(projectRoute);
        ValueTree payload(ApiKeys::Projects::project);
        // head reference will be put later when all revisions are pushed
        payload.setProperty(ApiKeys::Projects::title, this->projectName, nullptr);
        this->response = createProjectRequest.put(payload);
        if (!this->response.is2xx())
        {
            Logger::writeToLog("Failed to create the project on remote: " + this->response.getErrors().getFirst());
            callbackOnMessageThread(ProjectSyncThread, onSyncFailed, self->response.getErrors());
            return;
        }

        App::Workspace().getUserProfile().onProjectRemoteInfoUpdated({ this->response.getBody() });
    }
    else if (!this->response.is200())
    {
        Logger::writeToLog("Failed to fetch project heads from remote: " + this->response.getErrors().getFirst());
        callbackOnMessageThread(ProjectSyncThread, onSyncFailed, self->response.getErrors());
        return;
    }

    // the info about what revisions are available remotely will be needed by revision tree:
    this->vcs->updateRemoteSyncCache(remoteProject.getRevisions());

    using RevisionDtosMap = FlatHashMap<String, RevisionDto, StringHash>;

    RevisionDtosMap remoteRevisions;
    for (const auto child : remoteProject.getRevisions())
    {
        remoteRevisions[child.getId()] = child;
    }

    // find all new revisions on the remote
    Array<RevisionDto> newRemoteRevisions;
    for (const auto &remoteRevision : remoteRevisions)
    {
        if (!localRevisions.contains(remoteRevision.second.getId()))
        {
            newRemoteRevisions.add(remoteRevision.second);
        }
    }

    // find all new revisions locally
    ReferenceCountedArray<VCS::Revision> newLocalRevisions;
    for (const auto &localRevision : localRevisions)
    {
        if (!remoteRevisions.contains(localRevision.second->getUuid()))
        {
            newLocalRevisions.add(localRevision.second);
        }
    }

    // everything is up to date
    if (newLocalRevisions.isEmpty() && newRemoteRevisions.isEmpty())
    {
        callbackOnMessageThread(ProjectSyncThread, onSyncDone, true);
        return;
    }

    // build tree(s) of shallow VCS::Revision from newRemoteRevisions list and append them to VCS
    const auto newRemoteTrees = ProjectSyncHelpers::constructNewRemoteTrees(newRemoteRevisions);
    for (auto subtree : newRemoteTrees)
    {
        this->vcs->appendSubtree(subtree.second, subtree.first);
    }

    // callback that fetch is done,
    callbackOnMessageThread(ProjectSyncThread, onFetchDone);

    // if anything is needed to pull, fetch all data for each, then update and callback
    for (const auto dto : newRemoteRevisions)
    {
        if (this->idsToSync.isEmpty() || // if empty, sync all
            this->idsToSync.contains(dto.getId()))
        {
            const String revisionRoute(ApiRoutes::projectRevision
                .replace(":projectId", this->projectId)
                .replace(":revisionId", dto.getId()));

            const BackendRequest revisionRequest(revisionRoute);
            this->response = revisionRequest.get();
            if (!this->response.is2xx())
            {
                Logger::writeToLog("Failed to fetch revision data: " + this->response.getErrors().getFirst());
                callbackOnMessageThread(ProjectSyncThread, onSyncFailed, self->response.getErrors());
                return;
            }

            const RevisionDto fullRevision(this->response.getBody());
            const auto revision = this->vcs->updateShallowRevisionData(fullRevision.getId(), fullRevision.getData());
        }
    }

    // if anything is needed to push,
    // build tree(s) from newLocalRevisions list
    const auto newLocalTrees = ProjectSyncHelpers::constructNewLocalTrees(newLocalRevisions);

    // push them recursively, starting from the root, so that
    // each pushed revision already has a valid remote parent
    for (auto *subtree : newLocalTrees)
    {
        this->pushSubtreeRecursively(subtree);
    }

    // finally, update project head ref
    const BackendRequest createProjectRequest(projectRoute);
    ValueTree payload(ApiKeys::Projects::project);
    // head reference will be put later when all revisions are pushed
    payload.setProperty(ApiKeys::Projects::title, this->projectName, nullptr);
    payload.setProperty(ApiKeys::Projects::head, this->vcs->getHead().getHeadingRevision()->getUuid(), nullptr);
    this->response = createProjectRequest.put(payload);
    if (!this->response.is2xx())
    {
        Logger::writeToLog("Failed to update the project on remote: " + this->response.getErrors().getFirst());
        callbackOnMessageThread(ProjectSyncThread, onSyncFailed, self->response.getErrors());
        return;
    }

    callbackOnMessageThread(ProjectSyncThread, onSyncDone, false);
}

void ProjectSyncThread::pushSubtreeRecursively(VCS::Revision::Ptr root)
{
    if (this->idsToSync.isEmpty() ||
        this->idsToSync.contains(root->getUuid()))
    {
        const String revisionRoute(ApiRoutes::projectRevision
            .replace(":projectId", this->projectId)
            .replace(":revisionId", root->getUuid()));

        ValueTree payload(ApiKeys::Revisions::revision);
        payload.setProperty(ApiKeys::Revisions::message, root->getMessage(), nullptr);
        payload.setProperty(ApiKeys::Revisions::timestamp, String(root->getTimeStamp()), nullptr);
        payload.setProperty(ApiKeys::Revisions::parentId, (root->getParent() ? root->getParent()->getUuid() : var::null), nullptr);

        ValueTree data(ApiKeys::Revisions::data);
        data.addChild(root->serialize(), 0, nullptr);
        payload.addChild(data, 0, nullptr);

        const BackendRequest revisionRequest(revisionRoute);
        this->response = revisionRequest.put(payload);
        if (!this->response.is2xx())
        {
            Logger::writeToLog("Failed to put revision data: " + this->response.getErrors().getFirst());
            callbackOnMessageThread(ProjectSyncThread, onSyncFailed, self->response.getErrors());
            return;
        }

        // notify vcs that revision is available remotely
        this->vcs->updateLocalSyncCache(root);
    }

    for (auto *child : root->getChildren())
    {
        this->pushSubtreeRecursively(child);
    }
}
