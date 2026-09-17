// SPDX-FileCopyrightText: 2026 drippu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Headless-enough Apple smoke: create a Qt VulkanSurface window, attach a
// CAMetalLayer via the live suyu GetWindowSystemInfo path, then create a
// MoltenVK instance and metal surface. Does not boot a game.

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <QGuiApplication>
#include <QStringLiteral>
#include <QWindow>

#include "common/logging.h"
#include "suyu/qt_common.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_surface.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "vulkan/vulkan_core.h"

namespace {

void Fail(const std::string& why) {
    std::cerr << "macos_moltenvk_smoke: FAIL " << why << '\n';
    std::exit(1);
}

} // namespace

int main(int argc, char** argv) {
    Common::Log::Initialize();
    Common::Log::Start();
    Common::Log::SetColorConsoleBackendEnabled(true);

    QGuiApplication app(argc, argv);
    QWindow window;
    window.setTitle(QStringLiteral("macos_moltenvk_smoke"));
    window.resize(64, 64);
    window.setSurfaceType(QWindow::VulkanSurface);
    window.show();
    if (!window.create()) {
        Fail("QWindow::create failed");
    }
    QGuiApplication::processEvents();

    const auto wsi = QtCommon::GetWindowSystemInfo(&window);
    if (wsi.type != Core::Frontend::WindowSystemType::Cocoa) {
        Fail("window system is not Cocoa");
    }
    if (wsi.render_surface == nullptr) {
        Fail("GetWindowSystemInfo returned a null CAMetalLayer");
    }
    std::cout << "macos_moltenvk_smoke: CAMetalLayer ok\n";

    try {
        Vulkan::vk::InstanceDispatch dld;
        const auto library = Vulkan::OpenLibrary();
        const Vulkan::vk::Instance instance =
            Vulkan::CreateInstance(*library, dld, VK_API_VERSION_1_1, wsi.type);
        const Vulkan::vk::SurfaceKHR surface = Vulkan::CreateSurface(instance, wsi);
        std::cout << "macos_moltenvk_smoke: Vulkan surface ok\n";

        const std::vector<VkPhysicalDevice> physical_devices = instance.EnumeratePhysicalDevices();
        if (physical_devices.empty()) {
            Fail("no Vulkan physical devices");
        }

        bool found_moltenvk = false;
        for (const VkPhysicalDevice device : physical_devices) {
            const auto physical = Vulkan::vk::PhysicalDevice(device, dld);
            VkPhysicalDeviceDriverProperties driver_properties{};
            driver_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            VkPhysicalDeviceProperties2 properties{};
            properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties.pNext = &driver_properties;
            if (!dld.vkGetPhysicalDeviceProperties2) {
                Fail("vkGetPhysicalDeviceProperties2 is null");
            }
            dld.vkGetPhysicalDeviceProperties2(physical, &properties);

            const std::string driver_name = Vulkan::vk::GetDriverName(driver_properties);
            const std::string device_name = properties.properties.deviceName;
            std::cout << "macos_moltenvk_smoke: device=" << device_name
                      << " driver=" << driver_name << '\n';

            if (driver_properties.driverID == VK_DRIVER_ID_MOLTENVK ||
                driver_name.find("MoltenVK") != std::string::npos) {
                found_moltenvk = true;
                std::cout << "macos_moltenvk_smoke: driver=MoltenVK\n";
            }
        }

        if (!found_moltenvk) {
            Fail("no MoltenVK physical device");
        }
    } catch (const Vulkan::vk::Exception& exception) {
        Fail(std::string("Vulkan: ") + exception.what());
    } catch (const std::exception& exception) {
        Fail(std::string("exception: ") + exception.what());
    }

    std::cout << "macos_moltenvk_smoke: OK\n";
    Common::Log::Stop();
    return 0;
}
