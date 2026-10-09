#include <cstdint>
#include "prx/common/StderrLog.hpp"
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/AppMetadata/include/AppMetadata.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <filesystem>
#include <stdexcept>
#include <string>

static constexpr int SCE_APP_CONTENT_ERROR_PARAMETER = static_cast<int>(0x80D90002);
static constexpr uint32_t APPPARAM_ID_SKU_FLAG = 1;
static constexpr int32_t SKU_FLAG_FULL = 3;

static constexpr char TEMPORARY_MOUNT_POINT[] = "/temp0";
static constexpr char DOWNLOAD_MOUNT_POINT[] = "/download0";
static constexpr uint32_t TEMPORARY_DATA_OPTION_FORMAT = 1;

static std::filesystem::path TemporaryDirectory(const AppContentMountPoint* mount_point) {
    if (!mount_point || std::strncmp(mount_point->data, TEMPORARY_MOUNT_POINT, sizeof(mount_point->data)) != 0) APS5_INVALID_ARG_EX;
    return ResolvePath_nid_no_patch(TEMPORARY_MOUNT_POINT);
}

static void ClearDirectory(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) std::filesystem::remove_all(entry.path());
}

// Error codes as the PS4 and PS5 AppContent libraries number them.
static constexpr int SCE_APP_CONTENT_ERROR_NOT_MOUNTED = static_cast<int>(0x80D90004);
static constexpr int SCE_APP_CONTENT_ERROR_NOT_FOUND = static_cast<int>(0x80D90005);
static constexpr int SCE_APP_CONTENT_ERROR_MOUNT_FULL = static_cast<int>(0x80D90006);

// An installed add-on with data lives in addcont/<entitlement label> beside the runtime, usually a junction the client made
// to the add-on's own folder. A mount gives it the guest path /addcont<n>.
static constexpr char ADDCONT_DIRECTORY[] = "/addcont";
static constexpr std::size_t ADDCONT_MOUNT_MAX = 64;

static std::mutex& AddcontMutex() {
    static std::mutex mutex;
    return mutex;
}

// The entitlement label mounted at each /addcont<n>, empty for a free one.
static std::array<std::string, ADDCONT_MOUNT_MAX>& MountedAddcont() {
    static std::array<std::string, ADDCONT_MOUNT_MAX> mounted;
    return mounted;
}

static std::string AddcontMountPoint(std::size_t index) { return std::string(ADDCONT_DIRECTORY) + std::to_string(index); }

extern "C" {

int APS5_VABI sceAppContentAddcontMount(uint32_t service_label, const NpUnifiedEntitlementLabel* entitlement_label, AppContentMountPoint* mount_point) {
    (void)service_label;
    if (!entitlement_label || !mount_point) return SCE_APP_CONTENT_ERROR_PARAMETER;
    const std::string label(entitlement_label->data, strnlen(entitlement_label->data, sizeof(entitlement_label->data)));
    if (label.empty() || label.find_first_of("/\\.") != std::string::npos) return SCE_APP_CONTENT_ERROR_PARAMETER;
    const auto directory = ResolvePath_nid_no_patch(ADDCONT_DIRECTORY) / label;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return SCE_APP_CONTENT_ERROR_NOT_FOUND;
    std::lock_guard lock(AddcontMutex());
    auto& mounted = MountedAddcont();
    auto slot = std::find(mounted.begin(), mounted.end(), label);
    if (slot == mounted.end()) slot = std::find(mounted.begin(), mounted.end(), std::string{});
    if (slot == mounted.end()) return SCE_APP_CONTENT_ERROR_MOUNT_FULL;
    const auto point = AddcontMountPoint(static_cast<std::size_t>(slot - mounted.begin()));
    if (slot->empty()) {
        AddPathAlias_nid_no_patch(point.c_str(), std::filesystem::absolute(directory).string().c_str());
        *slot = label;
        aps5::LogErr("[appcontent] add-on %s mounted at %s\n", label.c_str(), point.c_str());
    }
    std::memset(mount_point->data, 0, sizeof(mount_point->data));
    std::memcpy(mount_point->data, point.c_str(), point.size());
    return 0;
}

int APS5_VABI sceAppContentAddcontUnmount(const AppContentMountPoint* mount_point) {
    if (!mount_point) return SCE_APP_CONTENT_ERROR_PARAMETER;
    const std::string point(mount_point->data, strnlen(mount_point->data, sizeof(mount_point->data)));
    std::lock_guard lock(AddcontMutex());
    auto& mounted = MountedAddcont();
    for (std::size_t index = 0; index < mounted.size(); ++index) {
        if (mounted[index].empty() || AddcontMountPoint(index) != point) continue;
        RemovePathAlias_nid_no_patch(point.c_str());
        mounted[index].clear();
        return 0;
    }
    return SCE_APP_CONTENT_ERROR_NOT_MOUNTED;
}

int APS5_VABI sceAppContentAppParamGetInt(uint32_t param_id, int32_t* value) {
    if (!value) return SCE_APP_CONTENT_ERROR_PARAMETER;
    switch (param_id) {
    case APPPARAM_ID_SKU_FLAG:
        *value = SKU_FLAG_FULL;
        return 0;
    case 2: case 3: case 4: case 5:
        *value = 0;
        return 0;
    default:
        return SCE_APP_CONTENT_ERROR_PARAMETER;
    }
}

int APS5_VABI sceAppContentDownloadDataGetAvailableSpaceKb(const AppContentMountPoint* mount_point, size_t* available_space_kb) {
    if (!available_space_kb) return SCE_APP_CONTENT_ERROR_PARAMETER;
    if (!mount_point || std::strncmp(mount_point->data, DOWNLOAD_MOUNT_POINT, sizeof(mount_point->data)) != 0) APS5_INVALID_ARG_EX;
    const std::uint64_t quotaKb = GetAppDownloadDataSizeMiB_nid_postfix() * 1024u;
    const auto directory = ResolvePath_nid_no_patch(DOWNLOAD_MOUNT_POINT);
    std::filesystem::create_directories(directory);
    std::uint64_t usedKb = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file()) usedKb += (entry.file_size() + 1023u) / 1024u;
    }
    const std::uint64_t hostKb = std::filesystem::space(directory).available / 1024u;

    if (quotaKb == 0) {
        static bool warned;
        if (!warned) {
            warned = true;
            aps5::LogErr( "[appcontent] the title declares no download data, %s reports host free space\n", __func__);
            aps5::LogFlush(aps5::LogStdErr);
        }
        *available_space_kb = static_cast<size_t>(hostKb);
        return 0;
    }
    *available_space_kb = static_cast<size_t>(std::min(quotaKb - std::min(quotaKb, usedKb), hostKb));
    return 0;
}

int APS5_VABI sceAppContentInitialize(const AppContentInitParam* init_param, AppContentBootParam* boot_param) {
    (void)init_param;
    if (!boot_param) return SCE_APP_CONTENT_ERROR_PARAMETER;
    std::memset(boot_param, 0, sizeof(*boot_param));
    return 0;
}

int APS5_VABI sceAppContentTemporaryDataFormat(const AppContentMountPoint* mount_point) {
    ClearDirectory(TemporaryDirectory(mount_point));
    return 0;
}

int APS5_VABI sceAppContentTemporaryDataGetAvailableSpaceKb(const AppContentMountPoint* mount_point, size_t* available_space_kb) {
    if (!available_space_kb) return SCE_APP_CONTENT_ERROR_PARAMETER;
    *available_space_kb = static_cast<size_t>(std::filesystem::space(TemporaryDirectory(mount_point)).available / 1024);
    return 0;
}

int APS5_VABI sceAppContentTemporaryDataMount2(uint32_t option, AppContentMountPoint* mount_point) {
    if (!mount_point) return SCE_APP_CONTENT_ERROR_PARAMETER;
    if (option > TEMPORARY_DATA_OPTION_FORMAT) throw std::invalid_argument(std::string(__func__) + ": unknown option " + std::to_string(option));
    std::memset(mount_point->data, 0, sizeof(mount_point->data));
    std::memcpy(mount_point->data, TEMPORARY_MOUNT_POINT, sizeof(TEMPORARY_MOUNT_POINT));
    const auto directory = TemporaryDirectory(mount_point);
    std::filesystem::create_directories(directory);
    if (option == TEMPORARY_DATA_OPTION_FORMAT) ClearDirectory(directory);
    return 0;
}

}
