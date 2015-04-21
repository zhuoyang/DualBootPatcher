/*
 * Copyright (C) 2014  Andrew Gunnerson <andrewgunnerson@gmail.com>
 *
 * This file is part of MultiBootPatcher
 *
 * MultiBootPatcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MultiBootPatcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MultiBootPatcher.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "patchers/multibootpatcher.h"

#include <cassert>

#include <unordered_set>

#include <archive.h>
#include <archive_entry.h>

#include <zip.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "bootimage.h"
#include "cpiofile.h"
#include "patcherconfig.h"
#include "private/fileutils.h"
#include "private/logging.h"


namespace mbp
{

/*! \cond INTERNAL */
class MultiBootPatcher::Impl
{
public:
    Impl(MultiBootPatcher *parent) : m_parent(parent) {}

    PatcherConfig *pc;
    const FileInfo *info;

    uint64_t bytes;
    uint64_t maxBytes;
    uint64_t files;
    uint64_t maxFiles;

    volatile bool cancelled;

    PatcherError error;

    // Callbacks
    ProgressUpdatedCallback progressCb;
    FilesUpdatedCallback filesCb;
    DetailsUpdatedCallback detailsCb;
    void *userData;

    // Patching
    struct zip *zInput = nullptr;
    struct zip *zOutput = nullptr;
    std::vector<AutoPatcher *> autoPatchers;

    bool patchBootImage(std::vector<unsigned char> *data);
    bool patchZip();

    bool pass1(struct zip * const zOutput,
               const std::string &temporaryDir,
               const std::unordered_set<std::string> &exclude);
    bool pass2(struct zip * const zOutput,
               const std::string &temporaryDir,
               const std::unordered_set<std::string> &files);
    bool openInputArchive();
    void closeInputArchive();
    bool openOutputArchive();
    void closeOutputArchive();

    void updateProgress(uint64_t bytes, uint64_t maxBytes);
    void updateFiles(uint64_t files, uint64_t maxFiles);
    void updateDetails(const std::string &msg);

    static void laProgressCb(uint64_t bytes, void *userData);

    std::string createTable();
    std::string createInfoProp();

private:
    MultiBootPatcher *m_parent;
};
/*! \endcond */


const std::string MultiBootPatcher::Id("MultiBootPatcher");


MultiBootPatcher::MultiBootPatcher(PatcherConfig * const pc)
    : m_impl(new Impl(this))
{
    m_impl->pc = pc;
}

MultiBootPatcher::~MultiBootPatcher()
{
}

PatcherError MultiBootPatcher::error() const
{
    return m_impl->error;
}

std::string MultiBootPatcher::id() const
{
    return Id;
}

bool MultiBootPatcher::usesPatchInfo() const
{
    return true;
}

void MultiBootPatcher::setFileInfo(const FileInfo * const info)
{
    m_impl->info = info;
}

std::string MultiBootPatcher::newFilePath()
{
    assert(m_impl->info != nullptr);

    boost::filesystem::path path(m_impl->info->filename());
    boost::filesystem::path fileName = path.stem();
    fileName += "_patched";
    fileName += path.extension();

    if (path.has_parent_path()) {
        return (path.parent_path() / fileName).string();
    } else {
        return fileName.string();
    }
}

void MultiBootPatcher::cancelPatching()
{
    m_impl->cancelled = true;
}

bool MultiBootPatcher::patchFile(ProgressUpdatedCallback progressCb,
                                 FilesUpdatedCallback filesCb,
                                 DetailsUpdatedCallback detailsCb,
                                 void *userData)
{
    m_impl->cancelled = false;

    assert(m_impl->info != nullptr);

    if (!boost::iends_with(m_impl->info->filename(), ".zip")) {
        m_impl->error = PatcherError::createSupportedFileError(
                ErrorCode::OnlyZipSupported, Id);
        return false;
    }

    m_impl->progressCb = progressCb;
    m_impl->filesCb = filesCb;
    m_impl->detailsCb = detailsCb;
    m_impl->userData = userData;

    m_impl->bytes = 0;
    m_impl->maxBytes = 0;
    m_impl->files = 0;
    m_impl->maxFiles = 0;

    bool ret = m_impl->patchZip();

    m_impl->progressCb = nullptr;
    m_impl->filesCb = nullptr;
    m_impl->detailsCb = nullptr;
    m_impl->userData = nullptr;

    for (auto *p : m_impl->autoPatchers) {
        m_impl->pc->destroyAutoPatcher(p);
    }
    m_impl->autoPatchers.clear();

    //if (m_impl->zInput != nullptr) {
    //    m_impl->closeInputArchive();
    //}
    if (m_impl->zOutput != nullptr) {
        m_impl->closeOutputArchive();
    }
    if (m_impl->zInput != nullptr) {
        m_impl->closeInputArchive();
    }

    if (m_impl->cancelled) {
        m_impl->error = PatcherError::createCancelledError(
                ErrorCode::PatchingCancelled);
        return false;
    }

    return ret;
}

bool MultiBootPatcher::Impl::patchBootImage(std::vector<unsigned char> *data)
{
    BootImage bi;
    if (!bi.load(*data)) {
        error = bi.error();
        return false;
    }

    // Release memory since BootImage keeps a copy of the separate components
    data->clear();
    data->shrink_to_fit();

    // Load the ramdisk cpio
    CpioFile cpio;
    if (!cpio.load(bi.ramdiskImage())) {
        error = cpio.error();
        return false;
    }

    if (cancelled) return false;

    auto *rp = pc->createRamdiskPatcher(
            info->patchInfo()->ramdisk(), info, &cpio);
    if (!rp) {
        error = PatcherError::createPatcherCreationError(
                ErrorCode::RamdiskPatcherCreateError,
                info->patchInfo()->ramdisk());
        return false;
    }

    if (!rp->patchRamdisk()) {
        error = rp->error();
        pc->destroyRamdiskPatcher(rp);
        return false;
    }

    pc->destroyRamdiskPatcher(rp);

    if (cancelled) return false;

    // Add mbtool
    const std::string mbtool("mbtool");

    if (cpio.exists(mbtool)) {
        cpio.remove(mbtool);
    }

    if (!cpio.addFile(pc->dataDirectory() + "/binaries/android/"
            + info->device()->architecture() + "/mbtool", mbtool, 0750)) {
        error = cpio.error();
        return false;
    }

    if (cancelled) return false;

    std::vector<unsigned char> newRamdisk;
    if (!cpio.createData(&newRamdisk)) {
        error = cpio.error();
        return false;
    }
    bi.setRamdiskImage(std::move(newRamdisk));

    // Reapply Bump if needed
    bi.setApplyBump(bi.wasBump());

    *data = bi.create();

    if (cancelled) return false;

    return true;
}

bool MultiBootPatcher::Impl::patchZip()
{
    std::unordered_set<std::string> excludeFromPass1;

    for (auto const &item : info->patchInfo()->autoPatchers()) {
        auto args = info->patchInfo()->autoPatcherArgs(item);

        auto *ap = pc->createAutoPatcher(item, info, args);
        if (!ap) {
            error = PatcherError::createPatcherCreationError(
                    ErrorCode::AutoPatcherCreateError, item);
            return false;
        }

        // TODO: AutoPatcher::newFiles() is not supported yet

        std::vector<std::string> existingFiles = ap->existingFiles();
        if (existingFiles.empty()) {
            pc->destroyAutoPatcher(ap);
            continue;
        }

        autoPatchers.push_back(ap);

        // AutoPatcher files should be excluded from the first pass
        for (auto const &file : existingFiles) {
            excludeFromPass1.insert(file);
        }
    }

    // Unlike the old patcher, we'll write directly to the new file
    if (!openOutputArchive()) {
        return false;
    }

    if (cancelled) return false;

    FileUtils::ArchiveStats stats;
    auto result = FileUtils::laArchiveStats(info->filename(), &stats,
                                            std::vector<std::string>());
    if (!result) {
        error = result;
        return false;
    }

    maxBytes = stats.totalSize;

    if (cancelled) return false;

    // +1 for mbtool_recovery (update-binary)
    // +1 for aromawrapper.zip
    // +1 for bb-wrapper.sh
    // +1 for unzip
    // +1 for info.prop
    maxFiles = stats.files + 5;
    updateFiles(files, maxFiles);

    if (!openInputArchive()) {
        return false;
    }

    // Create temporary dir for extracted files for autopatchers
    std::string tempDir = FileUtils::createTemporaryDir(pc->tempDirectory());

    if (!pass1(zOutput, tempDir, excludeFromPass1)) {
        boost::filesystem::remove_all(tempDir);
        return false;
    }

    if (cancelled) return false;

    // On the second pass, run the autopatchers on the rest of the files

    if (!pass2(zOutput, tempDir, excludeFromPass1)) {
        boost::filesystem::remove_all(tempDir);
        return false;
    }

    boost::filesystem::remove_all(tempDir);

    if (cancelled) return false;

    updateFiles(++files, maxFiles);
    updateDetails("META-INF/com/google/android/update-binary");

    // Add mbtool_recovery
    result = FileUtils::lzAddFile(
            zOutput, "META-INF/com/google/android/update-binary",
            pc->dataDirectory() + "/binaries/android/"
                    + info->device()->architecture() + "/mbtool_recovery");
    if (!result) {
        error = result;
        return false;
    }

    if (cancelled) return false;

    updateFiles(++files, maxFiles);
    updateDetails("multiboot/aromawrapper.zip");

    // Add aromawrapper.zip
    result = FileUtils::lzAddFile(
            zOutput, "multiboot/aromawrapper.zip",
            pc->dataDirectory() + "/aromawrapper.zip");
    if (!result) {
        error = result;
        return false;
    }

    if (cancelled) return false;

    updateFiles(++files, maxFiles);
    updateDetails("multiboot/bb-wrapper.sh");

    // Add bb-wrapper.sh
    result = FileUtils::lzAddFile(
        zOutput, "multiboot/bb-wrapper.sh",
        pc->dataDirectory() + "/scripts/bb-wrapper.sh");
    if (!result) {
        error = result;
        return false;
    }

    if (cancelled) return false;

    updateFiles(++files, maxFiles);
    updateDetails("multiboot/unzip");

    // Add unzip.tar.xz
    result = FileUtils::lzAddFile(
            zOutput, "multiboot/unzip",
            pc->dataDirectory() + "/binaries/android/"
                    + info->device()->architecture() + "/unzip");
    if (!result) {
        error = result;
        return false;
    }

    if (cancelled) return false;

    updateFiles(++files, maxFiles);
    updateDetails("multiboot/info.prop");

    const std::string infoProp = createInfoProp();
    result = FileUtils::lzAddFile(
            zOutput, "multiboot/info.prop",
            std::vector<unsigned char>(infoProp.begin(), infoProp.end()));
    if (!result) {
        error = result;
        return false;
    }

    if (cancelled) return false;

    return true;
}

/*!
 * \brief First pass of patching operation
 *
 * This performs the following operations:
 *
 * - If the PatchInfo has the `hasBootImage` parameter set to true for the
 *   current file, then patch the boot images and copy them to the output file.
 * - Files needed by an AutoPatcher are extracted to the temporary directory.
 * - Otherwise, the file is copied directly to the output zip.
 */
bool MultiBootPatcher::Impl::pass1(struct zip * const zOutput,
                                   const std::string &temporaryDir,
                                   const std::unordered_set<std::string> &exclude)
{
    // Boot image params
    bool hasBootImage = info->patchInfo()->hasBootImage();
    auto piBootImages = info->patchInfo()->bootImages();

    struct zip_stat sb;
    zip_int64_t numFiles;

    numFiles = zip_get_num_entries(zInput, 0);
    if (numFiles < 0) {
        FLOGE("libzip: Failed to get number of entries: {}",
              zip_strerror(zInput));
        error = PatcherError::createArchiveError(
                ErrorCode::ArchiveReadHeaderError, std::string());
        return false;
    }

    for (zip_uint64_t i = 0; i < (zip_uint64_t) numFiles; ++i) {
        if (cancelled) return false;

        if (zip_stat_index(zInput, i, 0, &sb) < 0) {
            FLOGE("libzip: Failed to stat: {}", zip_strerror(zInput));
            error = PatcherError::createArchiveError(
                    ErrorCode::ArchiveReadHeaderError, std::string());
            return false;
        }

        std::string curFile = zip_get_name(zInput, i, 0);

        updateFiles(++files, maxFiles);
        updateDetails(curFile);

        // Skip files that should be patched and added in pass 2
        if (exclude.find(curFile) != exclude.end()) {
            if (!FileUtils::lzExtractFile(zInput, i, temporaryDir)) {
                error = PatcherError::createArchiveError(
                        ErrorCode::ArchiveReadDataError, curFile);
                return false;
            }
            continue;
        }

        // Try to patch the patchinfo-defined boot images as well as
        // files that end in a common boot image extension

        bool inList = std::find(piBootImages.begin(), piBootImages.end(),
                                curFile) != piBootImages.end();
        bool isExtImg = boost::ends_with(curFile, ".img");
        bool isExtLok = boost::ends_with(curFile, ".lok");
        // Boot images should be over about 30 MiB. This check is here so the
        // patcher won't try to read a multi-gigabyte system image into RAM
        bool isSizeOK = sb.size <= 30 * 1024 * 1024;

        if (hasBootImage && (inList || isExtImg || isExtLok) && isSizeOK) {
            // Load the file into memory
            std::vector<unsigned char> data;

            if (!FileUtils::lzReadToMemory(zInput, i, &data /*, &laProgressCb, this */)) {
                error = PatcherError::createArchiveError(
                        ErrorCode::ArchiveReadDataError, curFile);
                return false;
            }

            // If the file contains the boot image magic string, then
            // assume it really is a boot image and patch it
            if (data.size() >= 512) {
                const char *magic = BootImage::BootMagic;
                unsigned int size = BootImage::BootMagicSize;
                auto end = data.begin() + 512;
                auto it = std::search(data.begin(), end,
                                      magic, magic + size);
                if (it != end) {
                    if (!patchBootImage(&data)) {
                        return false;
                    }

                    // Update total size
                    maxBytes += (data.size() - sb.size);
                }
            }

            PatcherError ret = FileUtils::lzAddFile(zOutput, curFile, data);
            if (!ret) {
                error = ret;
                return false;
            }

            bytes += data.size();
        } else {
            // Directly copy other files to the output zip

            // Rename the installer for mbtool
            if (curFile == "META-INF/com/google/android/update-binary") {
                curFile = "META-INF/com/google/android/update-binary.orig";
            }

            PatcherError ret = FileUtils::lzCopyDataDirect(
                    zInput, zOutput, i, curFile);
            if (!ret) {
                error = ret;
                return false;
            }

            bytes += sb.size;
        }
    }

    if (cancelled) return false;

    return true;
}

/*!
 * \brief Second pass of patching operation
 *
 * This performs the following operations:
 *
 * - Patch files in the temporary directory using the AutoPatchers and add the
 *   resulting files to the output zip
 */
bool MultiBootPatcher::Impl::pass2(struct zip * const zOutput,
                                   const std::string &temporaryDir,
                                   const std::unordered_set<std::string> &files)
{
    for (auto *ap : autoPatchers) {
        if (cancelled) return false;
        if (!ap->patchFiles(temporaryDir)) {
            error = ap->error();
            return false;
        }
    }

    // TODO Headers are being discarded

    for (auto const &file : files) {
        if (cancelled) return false;

        std::string path(temporaryDir);
        path += "/";
        path += file;

        PatcherError ret;

        if (file == "META-INF/com/google/android/update-binary") {
            ret = FileUtils::lzAddFile(
                    zOutput,
                    "META-INF/com/google/android/update-binary.orig",
                    path);
        } else {
            ret = FileUtils::lzAddFile(
                    zOutput,
                    file,
                    path);
        }

        if (!ret) {
            error = ret;
            return false;
        }
    }

    if (cancelled) return false;

    return true;
}

bool MultiBootPatcher::Impl::openInputArchive()
{
    assert(zInput == nullptr);

    char errorStr[1024];
    int errorId = 0;

    zInput = zip_open(info->filename().c_str(), 0, &errorId);
    if (!zInput) {
        zip_error_to_str(errorStr, sizeof(errorStr), errorId, errno);
        FLOGW("libzip: Failed to open input archive: {}", errorStr);

        error = PatcherError::createArchiveError(
                ErrorCode::ArchiveReadOpenError, info->filename());
        return false;
    }

    return true;
}

void MultiBootPatcher::Impl::closeInputArchive()
{
    assert(zInput != nullptr);

    if (zip_close(zInput) < 0) {
        FLOGW("libzip: Failed to close input archive: {}",
              zip_strerror(zInput));
    }

    zInput = nullptr;
}

bool MultiBootPatcher::Impl::openOutputArchive()
{
    assert(zOutput == nullptr);

    char errorStr[1024];
    int errorId = 0;

    const std::string newPath = m_parent->newFilePath();
    zOutput = zip_open(newPath.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorId);
    if (!zOutput) {
        zip_error_to_str(errorStr, sizeof(errorStr), errorId, errno);
        FLOGW("libzip: Failed to open output archive: {}", errorStr);

        error = PatcherError::createArchiveError(
                ErrorCode::ArchiveWriteOpenError, newPath);
        return false;
    }

    return true;
}

void MultiBootPatcher::Impl::closeOutputArchive()
{
    assert(zOutput != nullptr);

    if (zip_close(zOutput) < 0) {
        FLOGW("libzip: Failed to close output archive: {}",
              zip_strerror(zOutput));
    }

    zOutput = nullptr;
}

void MultiBootPatcher::Impl::updateProgress(uint64_t bytes, uint64_t maxBytes)
{
    if (progressCb) {
        progressCb(bytes, maxBytes, userData);
    }
}

void MultiBootPatcher::Impl::updateFiles(uint64_t files, uint64_t maxFiles)
{
    if (filesCb) {
        filesCb(files, maxFiles, userData);
    }
}

void MultiBootPatcher::Impl::updateDetails(const std::string &msg)
{
    if (detailsCb) {
        detailsCb(msg, userData);
    }
}

void MultiBootPatcher::Impl::laProgressCb(uint64_t bytes, void *userData)
{
    Impl *impl = static_cast<Impl *>(userData);
    impl->updateProgress(impl->bytes + bytes, impl->maxBytes);
}

template<typename SomeType, typename Predicate>
inline std::size_t insertAndFindMax(const std::vector<SomeType> &list1,
                                    std::vector<std::string> &list2,
                                    Predicate pred)
{
    std::size_t max = 0;
    for (const SomeType &st : list1) {
        std::size_t temp = pred(st, list2);
        if (temp > max) {
            max = temp;
        }
    }
    return max;
}

std::string MultiBootPatcher::Impl::createTable()
{
    std::string out;

    auto devices = pc->devices();
    std::vector<std::string> ids;
    std::vector<std::string> codenames;
    std::vector<std::string> names;

    std::size_t maxLenId = insertAndFindMax(devices, ids,
            [](Device *d, std::vector<std::string> &list) {
                list.push_back(d->id());
                return d->id().size();
            });
    std::size_t maxLenCodenames = insertAndFindMax(devices, codenames,
            [](Device *d, std::vector<std::string> &list) {
                auto codenames = d->codenames();
                std::string out;
                bool first = true;

                for (auto const codename : codenames) {
                    if (first) {
                        first = false;
                        out += codename;
                    } else {
                        out += ", ";
                        out += codename;
                    }
                }

                std::size_t len = out.size();
                list.push_back(std::move(out));
                return len;
            });
    std::size_t maxLenName = insertAndFindMax(devices, names,
            [](Device *d, std::vector<std::string> &list) {
                list.push_back(d->name());
                return d->name().size();
            });

    const std::string titleDevice = "Device";
    const std::string titleCodenames = "Codenames";
    const std::string titleName = "Name";

    if (titleDevice.size() > maxLenId) {
        maxLenId = titleDevice.size();
    }
    if (titleCodenames.size() > maxLenCodenames) {
        maxLenCodenames = titleCodenames.size();
    }
    if (titleName.size() > maxLenName) {
        maxLenName = titleName.size();
    }

    const std::string rowFmt =
            fmt::format("# | {{:<{}}} | {{:<{}}} | {{:<{}}} |\n",
                        maxLenId, maxLenCodenames, maxLenName);
    const std::string sepFmt =
            fmt::format("# |{{:-<{}}}|{{:-<{}}}|{{:-<{}}}|\n",
                        maxLenId + 2, maxLenCodenames + 2, maxLenName + 2);

    // Titles
    out += fmt::format(rowFmt, titleDevice, titleCodenames, titleName);

    // Separator
    out += fmt::format(sepFmt, std::string(), std::string(), std::string());

    // Devices
    for (std::size_t i = 0; i < devices.size(); ++i) {
        out += fmt::format(rowFmt, ids[i], codenames[i], names[i]);
    }

    return out;
}

std::string MultiBootPatcher::Impl::createInfoProp()
{
    std::string out;

    out +=
"# [Autogenerated by libmbp]\n"
"#\n"
"# Blank lines are ignored. Lines beginning with '#' are comments and are also\n"
"# ignored. Before changing any fields, please read its description. Invalid\n"
"# values may lead to unexpected behavior when this zip file is installed.\n"
"\n"
"\n"
"# mbtool.installer.version\n"
"# ------------------------\n"
"# This field is the version of libmbp and mbtool used to patch and install this\n"
"# file, respectively.\n"
"#\n";

    out += "mbtool.installer.version=";
    out += pc->version();
    out += "\n";

    out +=
"\n"
"\n"
"# mbtool.installer.device\n"
"# -----------------------\n"
"# This field specifies the target device for this zip file. Based on the value,\n"
"# mbtool will determine the appropriate partitions to use as well as other\n"
"# device-specific operations (eg. Loki for locked Galaxy S4 and LG G2\n"
"# bootloaders). The devices supported by mbtool are specified below.\n"
"#\n"
"# WARNING: Except for debugging purposes, this value should NEVER be changed.\n"
"# An incorrect value can hard-brick the device due to differences in the\n"
"# partition table.\n"
"#\n"
"# Supported devices:\n"
"#\n";

    out += createTable();
    out += "#\n";
    out += "mbtool.installer.device=";
    out += info->device()->id();
    out += "\n";

    out +=
"\n"
"\n"
"# mbtool.installer.ignore-codename\n"
"# --------------------------------\n"
"# The installer checks the device by comparing the devices codenames to the\n"
"# valid codenames in the table above. This value is useful when the device is\n"
"# a variant of a supported device (or very similar to one).\n"
"#\n"
"# For example, if 'mbtool.installer.device' is set to 'trlte' and this field is\n"
"# set to true, then mbtool would not check to see if the device's codename is\n"
"# 'trltetmo' or 'trltexx'.\n"
"#\n"
"mbtool.installer.ignore-codename=false\n"
"\n"
"\n"
"# mbtool.installer.install-location\n"
"# ---------------------------------\n"
"# If this field is set, mbtool will not show the AROMA selection screen for\n"
"# choosing the installation location. If an invalid value is specified, then the\n"
"# installation will fail. This is disabled by default.\n"
"#\n"
"# This value is completely ignored if this zip is flashed from the app. It only\n"
"# applies when flashing from recovery.\n"
"#\n"
"# Valid values: primary, dual, multi-slot-1, multi-slot-2, multi-slot-3\n"
"#\n"
"#mbtool.installer.install-location=chosen_location\n"
"\n";

    return out;
}

}
