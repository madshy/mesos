// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __WINDOWS__
#include <fts.h>
#endif // __WINDOWS__

#include <mesos/type_utils.hpp>

#include <mesos/docker/spec.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/uuid.hpp>

#include "slave/paths.hpp"

#include "slave/containerizer/mesos/provisioner/backend.hpp"
#include "slave/containerizer/mesos/provisioner/paths.hpp"
#include "slave/containerizer/mesos/provisioner/provisioner.hpp"
#include "slave/containerizer/mesos/provisioner/store.hpp"

using namespace process;

namespace spec = docker::spec;

using std::list;
using std::string;
using std::vector;

using mesos::slave::ContainerState;

namespace mesos {
namespace internal {
namespace slave {

Try<Owned<Provisioner>> Provisioner::create(const Flags& flags)
{
  string _rootDir = slave::paths::getProvisionerDir(flags.work_dir);

  Try<Nothing> mkdir = os::mkdir(_rootDir);
  if (mkdir.isError()) {
    return Error(
        "Failed to create provisioner root directory '" +
        _rootDir + "': " + mkdir.error());
  }

  Result<string> rootDir = os::realpath(_rootDir);
  if (rootDir.isError()) {
    return Error(
        "Failed to resolve the realpath of provisioner root directory '" +
        _rootDir + "': " + rootDir.error());
  }

  CHECK_SOME(rootDir); // Can't be None since we just created it.

  Try<hashmap<Image::Type, Owned<Store>>> stores = Store::create(flags);
  if (stores.isError()) {
    return Error("Failed to create image stores: " + stores.error());
  }

  hashmap<string, Owned<Backend>> backends = Backend::create(flags);
  if (backends.empty()) {
    return Error("No usable provisioner backend created");
  }

  if (!backends.contains(flags.image_provisioner_backend)) {
    return Error(
        "The specified provisioner backend '" +
        flags.image_provisioner_backend + "' is unsupported");
  }

  return Owned<Provisioner>(new Provisioner(
      Owned<ProvisionerProcess>(new ProvisionerProcess(
          flags,
          rootDir.get(),
          stores.get(),
          backends))));
}


Provisioner::Provisioner(Owned<ProvisionerProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


Provisioner::~Provisioner()
{
  if (process.get() != nullptr) {
    terminate(process.get());
    wait(process.get());
  }
}


Future<Nothing> Provisioner::recover(
    const hashset<ContainerID>& knownContainerIds) const
{
  return dispatch(
      CHECK_NOTNULL(process.get()),
      &ProvisionerProcess::recover,
      knownContainerIds);
}


Future<ProvisionInfo> Provisioner::provision(
    const ContainerID& containerId,
    const Image& image) const
{
  return dispatch(
      CHECK_NOTNULL(process.get()),
      &ProvisionerProcess::provision,
      containerId,
      image);
}


Future<bool> Provisioner::destroy(const ContainerID& containerId) const
{
  return dispatch(
      CHECK_NOTNULL(process.get()),
      &ProvisionerProcess::destroy,
      containerId);
}


ProvisionerProcess::ProvisionerProcess(
    const Flags& _flags,
    const string& _rootDir,
    const hashmap<Image::Type, Owned<Store>>& _stores,
    const hashmap<string, Owned<Backend>>& _backends)
  : ProcessBase(process::ID::generate("mesos-provisioner")),
    flags(_flags),
    rootDir(_rootDir),
    stores(_stores),
    backends(_backends) {}


Future<Nothing> ProvisionerProcess::recover(
    const hashset<ContainerID>& knownContainerIds)
{
  // List provisioned containers, recover known ones, and destroy
  // unknown ones. Note that known orphan containers are recovered as
  // well and they will be destroyed by the containerizer using the
  // normal cleanup path. See MESOS-2367 for details.
  //
  // NOTE: All containers, including top level container and child
  // containers, will be included in the hashset.
  Try<hashset<ContainerID>> containers =
    provisioner::paths::listContainers(rootDir);

  if (containers.isError()) {
    return Failure(
        "Failed to list the containers managed by the provisioner: " +
        containers.error());
  }

  // Scan the list of containers, register all of them with 'infos'
  // but mark unknown containers for immediate cleanup.
  hashset<ContainerID> unknownContainerIds;

  foreach (const ContainerID& containerId, containers.get()) {
    Owned<Info> info = Owned<Info>(new Info());

    Try<hashmap<string, hashset<string>>> rootfses =
      provisioner::paths::listContainerRootfses(rootDir, containerId);

    if (rootfses.isError()) {
      return Failure(
          "Unable to list rootfses belonged to container " +
          stringify(containerId) + ": " + rootfses.error());
    }

    foreachkey (const string& backend, rootfses.get()) {
      if (!backends.contains(backend)) {
        return Failure(
            "Found rootfses managed by an unrecognized backend: " + backend);
      }

      info->rootfses.put(backend, rootfses.get()[backend]);
    }

    infos.put(containerId, info);

    if (knownContainerIds.contains(containerId)) {
      LOG(INFO) << "Recovered container " << containerId;
      continue;
    } else {
      // For immediate cleanup below.
      unknownContainerIds.insert(containerId);
    }
  }

  // Cleanup unknown orphan containers' rootfses.
  list<Future<bool>> cleanups;
  foreach (const ContainerID& containerId, unknownContainerIds) {
    LOG(INFO) << "Cleaning up unknown container " << containerId;

    // If a container is unknown, it means the launcher has not forked
    // it yet. So an unknown container should not have any child. It
    // means that when destroying an unknown container, we can just
    // simply call 'destroy' directly, without needing to make a
    // recursive call to destroy.
    cleanups.push_back(destroy(containerId));
  }

  Future<Nothing> cleanup = collect(cleanups)
    .then([]() -> Future<Nothing> { return Nothing(); });

  // Recover stores.
  list<Future<Nothing>> recovers;
  foreachvalue (const Owned<Store>& store, stores) {
    recovers.push_back(store->recover());
  }

  Future<Nothing> recover = collect(recovers)
    .then([]() -> Future<Nothing> { return Nothing(); });

  // A successful provisioner recovery depends on:
  //  1) Recovery of known containers (done above).
  //  2) Successful cleanup of unknown containers.
  //  `3) Successful store recovery.
  //
  // TODO(jieyu): Do not recover 'store' before unknown containers are
  // cleaned up. In the future, we may want to cleanup unused rootfses
  // in 'store', which might fail if there still exist unknown
  // containers holding references to them.
  return collect(cleanup, recover)
    .then([=]() -> Future<Nothing> {
      LOG(INFO) << "Provisioner recovery complete";
      return Nothing();
    });
}


Future<ProvisionInfo> ProvisionerProcess::provision(
    const ContainerID& containerId,
    const Image& image)
{
  if (!stores.contains(image.type())) {
    return Failure(
        "Unsupported container image type: " +
        stringify(image.type()));
  }

  // Get and then provision image layers from the store.
  return stores.get(image.type()).get()->get(image)
    .then(defer(self(), &Self::_provision, containerId, image, lambda::_1));
}


Future<ProvisionInfo> ProvisionerProcess::_provision(
    const ContainerID& containerId,
    const Image& image,
    const ImageInfo& imageInfo)
{
  // TODO(jieyu): Choose a backend smartly. For instance, if there is
  // only one layer returned from the store, prefer to use bind
  // backend because it's the simplest.
  const string& backend = flags.image_provisioner_backend;
  CHECK(backends.contains(backend));

  string rootfsId = UUID::random().toString();

  string rootfs = provisioner::paths::getContainerRootfsDir(
      rootDir,
      containerId,
      backend,
      rootfsId);

  LOG(INFO) << "Provisioning image rootfs '" << rootfs
            << "' for container " << containerId;

  // NOTE: It's likely that the container ID already exists in 'infos'
  // because one container might provision multiple images.
  if (!infos.contains(containerId)) {
    infos.put(containerId, Owned<Info>(new Info()));
  }

  infos[containerId]->rootfses[backend].insert(rootfsId);

  string backendDir = provisioner::paths::getBackendDir(
      rootDir,
      containerId,
      backend);

  return backends.get(backend).get()->provision(
      imageInfo.layers,
      rootfs,
      backendDir)
    .then(defer(self(), &Self::__provision, rootfs, image, imageInfo));
}


// This function is currently docker image specific. Depending
// on docker v1 spec, a docker image may include filesystem
// changeset, which may need to delete directories or files.
// The file/directory to be deleted will be labeled by creating
// a 'whiteout' file, which is at the same location and with the
// basename of the deleted file or directory prefixed with '.wh.'.
// For the directory which has an opaque whiteout file '.wh..wh..opq'
// under it, we need to delete all the files/directories under it.
// Please see:
// https://github.com/docker/docker/blob/master/image/spec/v1.md
// https://github.com/docker/docker/blob/master/pkg/archive/whiteouts.go
// And OCI image spec also has the concepts 'whiteout' and 'opaque whiteout':
// https://github.com/opencontainers/image-spec/blob/master/layer.md#whiteouts
Future<ProvisionInfo> ProvisionerProcess::__provision(
    const string& rootfs,
    const Image& image,
    const ImageInfo& imageInfo)
{
#ifdef __WINDOWS__
  return ProvisionInfo{
      rootfs, imageInfo.dockerManifest, imageInfo.appcManifest};
#else
  // Skip single-layered images since no 'whiteout' files needs
  // to be handled, and this excludes any image using the bind
  // backend.
  if (imageInfo.layers.size() == 1 || image.type() != Image::DOCKER) {
      return ProvisionInfo{
          rootfs, imageInfo.dockerManifest, imageInfo.appcManifest};
  }

  // TODO(hausdorff): The FTS API is not available on some platforms, such as
  // Windows. We will need to either (1) prove that this is not necessary for
  // Windows Containers, which use much of the Docker spec themselves, or (2)
  // make this code compatible with Windows, as we did with other code that
  // depended on FTS, such as `os::rmdir`. See MESOS-5610.
  char* _rootfs[] = {const_cast<char*>(rootfs.c_str()), nullptr};

  FTS* tree = ::fts_open(_rootfs, FTS_NOCHDIR | FTS_PHYSICAL, nullptr);
  if (tree == nullptr) {
    return Failure("Failed to open '" + rootfs + "': " + os::strerror(errno));
  }

  vector<string> whiteout;
  vector<string> whiteoutOpaque;

  for (FTSENT *node = ::fts_read(tree);
       node != nullptr; node = ::fts_read(tree)) {
    if (node->fts_info == FTS_F &&
        strings::startsWith(node->fts_name, string(spec::WHITEOUT_PREFIX))) {
      Path path = Path(node->fts_path);
      if (node->fts_name == string(spec::WHITEOUT_OPAQUE_PREFIX)) {
        whiteoutOpaque.push_back(path.dirname());
      } else {
        whiteout.push_back(path::join(
            path.dirname(),
            path.basename().substr(strlen(spec::WHITEOUT_PREFIX))));
      }

      Try<Nothing> rm = os::rm(path.string());
      if (rm.isError()) {
        ::fts_close(tree);
        return Failure(
            "Failed to remove whiteout file '" +
            path.string() + "': " + rm.error());
      }
    }
  }

  if (errno != 0) {
    Error error = ErrnoError();
    ::fts_close(tree);
    return Failure(error);
  }

  if (::fts_close(tree) != 0) {
    return Failure(
        "Failed to stop traversing file system: " + os::strerror(errno));
  }

  foreach (const string& path, whiteoutOpaque) {
    Try<Nothing> rmdir = os::rmdir(path, true, false);
    if (rmdir.isError()) {
      return Failure(
          "Failed to remove the entries under the directory labeled as"
          " opaque whiteout '" + path + "': " + rmdir.error());
    }
  }

  foreach (const string& path, whiteout) {
    // The file/directory labeled as whiteout may have already been
    // removed with the code above due to its parent directory labeled
    // as opaque whiteout, so here we need to check if it still exists
    // before trying to remove it.
    if (os::exists(path)) {
      if (os::stat::isdir(path)) {
        Try<Nothing> rmdir = os::rmdir(path);
        if (rmdir.isError()) {
          return Failure(
              "Failed to remove the directory labeled as whiteout '" +
              path + "': " + rmdir.error());
        }
      } else {
        Try<Nothing> rm = os::rm(path);
        if (rm.isError()) {
          return Failure(
              "Failed to remove the file labeled as whiteout '" +
              path + "': " + rm.error());
        }
      }
    }
  }

  return ProvisionInfo{rootfs, imageInfo.dockerManifest, None()};
#endif // __WINDOWS__
}


Future<bool> ProvisionerProcess::destroy(const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring destroy request for unknown container " << containerId;

    return false;
  }

  // Provisioner destroy can be invoked from:
  // 1. Provisioner `recover` to destroy all unknown orphans.
  // 2. Containerizer `recover` to destroy known orphans.
  // 3. Containerizer `destroy` on one specific container.
  //
  // In the above cases, we assume that the container being destroyed
  // has no corresponding child containers. We fail fast if this
  // condition is not satisfied.
  //
  // NOTE: This check is expensive since it traverses the entire
  // `infos` hashmap. This is acceptable because we generally expect
  // the number of containers on a single agent to be on the order of
  // tens or hundreds of containers.
  foreachkey (const ContainerID& entry, infos) {
    if (entry.has_parent()) {
      CHECK(entry.parent() != containerId)
        << "Failed to destroy container "
        << containerId << " since its nested container "
        << entry << " has not been destroyed yet";
    }
  }

  // Unregister the container first. If destroy() fails, we can rely
  // on recover() to retry it later.
  Owned<Info> info = infos[containerId];
  infos.erase(containerId);

  list<Future<bool>> futures;
  foreachkey (const string& backend, info->rootfses) {
    if (!backends.contains(backend)) {
      return Failure("Unknown backend '" + backend + "'");
    }

    foreach (const string& rootfsId, info->rootfses[backend]) {
      string rootfs = provisioner::paths::getContainerRootfsDir(
          rootDir,
          containerId,
          backend,
          rootfsId);

      string backendDir = provisioner::paths::getBackendDir(
          rootDir,
          containerId,
          backend);

      LOG(INFO) << "Destroying container rootfs at '" << rootfs
                << "' for container " << containerId;

      futures.push_back(
          backends.get(backend).get()->destroy(rootfs, backendDir));
    }
  }

  // TODO(xujyan): Revisit the usefulness of this return value.
  return collect(futures)
    .then(defer(self(), &ProvisionerProcess::_destroy, containerId));
}


Future<bool> ProvisionerProcess::_destroy(const ContainerID& containerId)
{
  // This should be fairly cheap as the directory should only
  // contain a few empty sub-directories at this point.
  //
  // TODO(jieyu): Currently, it's possible that some directories
  // cannot be removed due to EBUSY. EBUSY is caused by the race
  // between cleaning up this container and new containers copying
  // the host mount table. It's OK to ignore them. The cleanup
  // will be retried during slave recovery.
  string containerDir =
    provisioner::paths::getContainerDir(rootDir, containerId);

  Try<Nothing> rmdir = os::rmdir(containerDir);
  if (rmdir.isError()) {
    LOG(ERROR) << "Failed to remove the provisioned container directory "
               << "at '" << containerDir << "': " << rmdir.error();

    ++metrics.remove_container_errors;
  }

  return true;
}


ProvisionerProcess::Metrics::Metrics()
  : remove_container_errors(
      "containerizer/mesos/provisioner/remove_container_errors")
{
  process::metrics::add(remove_container_errors);
}


ProvisionerProcess::Metrics::~Metrics()
{
  process::metrics::remove(remove_container_errors);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
