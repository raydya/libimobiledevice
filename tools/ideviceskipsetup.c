/*
 * ideviceactivation.c
 * A command line tool to handle the activation process
 *
 * Copyright (c) 2016-2019 Nikias Bassen, All Rights Reserved.
 * Copyright (c) 2014-2015 Martin Szulecki, All Rights Reserved.
 * Copyright (c) 2011-2015 Mirell Development, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#define TOOL_NAME "ideviceskipsetup"

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#ifndef _MSC_VER

#include <unistd.h>

#endif

#include <ctype.h>

#ifndef WIN32

#include <signal.h>

#endif

#include <plist/plist.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/mobileactivation.h>
#include <libideviceactivation.h>
#include <libimobiledevice/mcinstall.h>


#ifdef WIN32
#include <windows.h>
#include <conio.h>
#else

#include <termios.h>

#endif

static int mc_read_from_file(const char *path, unsigned char **mc_data, unsigned int *mc_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Could not open file '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long int size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size >= 0x1000000) {
        fprintf(stderr, "The file '%s' is too large for processing.\n", path);
        fclose(f);
        return -1;
    }

    unsigned char *buf = malloc(size);
    if (!buf) {
        fprintf(stderr, "Could not allocate memory...\n");
        fclose(f);
        return -1;
    }

    long int cur = 0;
    while (cur < size) {
        ssize_t r = fread(buf + cur, 1, 512, f);
        if (r <= 0) {
            break;
        }
        cur += r;
    }
    fclose(f);

    if (cur != size) {
        free(buf);
        fprintf(stderr, "Could not read in file '%s' (size %ld read %ld)\n", path, size, cur);
        return -1;
    }

    *mc_data = buf;
    *mc_size = (unsigned int) size;

    return 0;
}

void get_cloud_config(plist_t *out) {
    plist_t dict_cloud = plist_new_dict();
    plist_dict_set_item(dict_cloud, "AllowPairing", plist_new_bool(1));
    plist_dict_set_item(dict_cloud, "CloudConfigurationUIComplete", plist_new_bool(1));
    plist_dict_set_item(dict_cloud, "ConfigurationSource", plist_new_bool(1));
    plist_dict_set_item(dict_cloud, "ConfigurationWasApplied", plist_new_bool(1));
    plist_dict_set_item(dict_cloud, "IsMDMUnremovable", plist_new_bool(0));
    plist_dict_set_item(dict_cloud, "IsMandatory", plist_new_bool(0));
    plist_dict_set_item(dict_cloud, "IsSupervised", plist_new_bool(0));
    plist_dict_set_item(dict_cloud, "PostSetupProfileWasInstalled", plist_new_bool(1));


    plist_t array_skip = plist_new_array();
    plist_array_append_item(array_skip, plist_new_string("Accessibility")); // +
    plist_array_append_item(array_skip, plist_new_string("AccessibilityAppearance"));
    plist_array_append_item(array_skip, plist_new_string("ActionButton")); // +
    plist_array_append_item(array_skip, plist_new_string("All"));
    plist_array_append_item(array_skip, plist_new_string("Android"));
    plist_array_append_item(array_skip, plist_new_string("Appearance"));
    plist_array_append_item(array_skip, plist_new_string("AppleID"));
    plist_array_append_item(array_skip, plist_new_string("AppStore")); // +
    plist_array_append_item(array_skip, plist_new_string("Biometric"));
    plist_array_append_item(array_skip, plist_new_string("CloudStorage"));
    plist_array_append_item(array_skip, plist_new_string("DeviceToDeviceMigration"));
    plist_array_append_item(array_skip, plist_new_string("Diagnostics"));
    plist_array_append_item(array_skip, plist_new_string("Display"));
    plist_array_append_item(array_skip, plist_new_string("DisplayTone"));
    plist_array_append_item(array_skip, plist_new_string("EnableLockdownMode")); // +
    plist_array_append_item(array_skip, plist_new_string("ExpressLanguage"));
    plist_array_append_item(array_skip, plist_new_string("FileVault"));
    plist_array_append_item(array_skip, plist_new_string("HomeButtonSensitivity"));
    plist_array_append_item(array_skip, plist_new_string("iCloudDiagnostics")); // +
    plist_array_append_item(array_skip, plist_new_string("iCloudStorage")); // +
    plist_array_append_item(array_skip, plist_new_string("iMessageAndFaceTime")); // +
    plist_array_append_item(array_skip, plist_new_string("Intelligence")); // +
    plist_array_append_item(array_skip, plist_new_string("IntendedUser"));
    plist_array_append_item(array_skip, plist_new_string("Keyboard"));
    plist_array_append_item(array_skip, plist_new_string("Language"));
    plist_array_append_item(array_skip, plist_new_string("LanguageAndLocale"));
    plist_array_append_item(array_skip, plist_new_string("Location"));
    plist_array_append_item(array_skip, plist_new_string("MessagingActivationUsingPhoneNumber"));
    plist_array_append_item(array_skip, plist_new_string("N/A"));
    plist_array_append_item(array_skip, plist_new_string("OnBoarding"));
    plist_array_append_item(array_skip, plist_new_string("Passcode"));
    plist_array_append_item(array_skip, plist_new_string("Payment"));
    plist_array_append_item(array_skip, plist_new_string("PreferredLanguage"));
    plist_array_append_item(array_skip, plist_new_string("Privacy"));
    plist_array_append_item(array_skip, plist_new_string("Region"));
    plist_array_append_item(array_skip, plist_new_string("Registration"));
    plist_array_append_item(array_skip, plist_new_string("Restore"));
    plist_array_append_item(array_skip, plist_new_string("RestoreCompleted"));
    plist_array_append_item(array_skip, plist_new_string("SIMSetup"));
    plist_array_append_item(array_skip, plist_new_string("Safety"));
    plist_array_append_item(array_skip, plist_new_string("ScreenSaver"));
    plist_array_append_item(array_skip, plist_new_string("ScreenTime"));
    plist_array_append_item(array_skip, plist_new_string("Siri"));
    plist_array_append_item(array_skip, plist_new_string("SoftwareUpdate"));
    plist_array_append_item(array_skip, plist_new_string("SpokenLanguage"));
    plist_array_append_item(array_skip, plist_new_string("TapToSetup"));
    plist_array_append_item(array_skip, plist_new_string("TermsOfAddress"));
    plist_array_append_item(array_skip, plist_new_string("Tone"));
    plist_array_append_item(array_skip, plist_new_string("TOS"));
    plist_array_append_item(array_skip, plist_new_string("TouchID"));
    plist_array_append_item(array_skip, plist_new_string("TrueToneDisplay"));
    plist_array_append_item(array_skip, plist_new_string("TVHomeScreenSync"));
    plist_array_append_item(array_skip, plist_new_string("TVProviderSignIn"));
    plist_array_append_item(array_skip, plist_new_string("TVRoom"));
    plist_array_append_item(array_skip, plist_new_string("UpdateCompleted"));
    plist_array_append_item(array_skip, plist_new_string("VoiceSelection")); // +
    plist_array_append_item(array_skip, plist_new_string("Wallpaper")); // +
    plist_array_append_item(array_skip, plist_new_string("WatchMigration"));
    plist_array_append_item(array_skip, plist_new_string("Welcome"));
    plist_array_append_item(array_skip, plist_new_string("WiFi"));
    plist_array_append_item(array_skip, plist_new_string("Zoom"));
    plist_dict_set_item(dict_cloud, "SkipSetup", array_skip);

    *out = plist_copy(dict_cloud);
    plist_free(dict_cloud);
}


int skip_setup(char *udid, char *mcFilename) {
    idevice_error_t ret = IDEVICE_E_UNKNOWN_ERROR;
    int result = EXIT_SUCCESS;

    idevice_t device = NULL;
    lockdownd_client_t lockdown = NULL;
    lockdownd_service_descriptor_t svc = NULL;
    mcinstall_client_t mis = NULL;

    ret = idevice_new_with_options(&device, udid, IDEVICE_LOOKUP_USBMUX);
    if (ret != IDEVICE_E_SUCCESS) {
        if (udid) {
            printf("ERROR: Device %s not found!\n", udid);
            free(udid);
        } else {
            printf("ERROR: No device found!\n");
        }
        return EXIT_FAILURE;
    }


    if (LOCKDOWN_E_SUCCESS != lockdownd_client_new_with_handshake(device, &lockdown, TOOL_NAME)) {
        fprintf(stderr, "Failed to connect to lockdownd\n");
        result = EXIT_FAILURE;
        goto cleanup;
    }

    lockdownd_error_t lockdown_ret = lockdownd_start_service(lockdown, "com.apple.mobile.MCInstall", &svc);
    if (LOCKDOWN_E_SUCCESS == lockdown_ret) {
        printf("start MCInstall service success\n");
    } else {
        result = EXIT_FAILURE;
        printf("failed to start service MCInstall\n");
        goto cleanup;
    }


    if (mcinstall_client_new(device, svc, &mis) != MCINSTALL_E_SUCCESS) {
        fprintf(stderr, "Could not connect to \"com.apple.mobile.MCInstall\" on device\n");
        result = EXIT_FAILURE;
        goto cleanup;
    }

    if (mcFilename && strlen(mcFilename) > 0 && mis) {
        unsigned char *mc_data = NULL;
        unsigned int mc_size = 0;
        if (mc_read_from_file(mcFilename, &mc_data, &mc_size) != 0) {
            if (mc_data) {
                free(mc_data);
            }
            goto cleanup;
        }

        plist_t payload = plist_new_data(mc_data, mc_size);
        if (mc_data) {
            free(mc_data);
        }

        if (plist_get_node_type(payload) == PLIST_DATA) {
            if (MCINSTALL_E_SUCCESS == mcinstall_install(mis, payload)) {
                printf("set wifi success\n");
            }
        }

        sleep(1000);    // sleep 1 second
    }

    lockdownd_set_value(lockdown, NULL, "TimeZone", plist_new_string("Asia/Shanghai"));
    lockdownd_set_value(lockdown, NULL, "Uses24HourClock", plist_new_bool(1));
    lockdownd_set_value(lockdown, "com.apple.international", "Locale", plist_new_string("zh_CN"));
    lockdownd_set_value(lockdown, "com.apple.international", "Language", plist_new_string("zh-Hans"));
    lockdownd_set_value(lockdown, "com.apple.purplebuddy", "SetupDone", plist_new_bool(1));
    lockdownd_set_value(lockdown, "com.apple.purplebuddy", "SetupFinishedAllSteps", plist_new_bool(1));
    lockdownd_set_value(lockdown, "com.apple.purplebuddy", "ForceNoBuddy", plist_new_bool(1));
    lockdownd_set_value(lockdown, "com.apple.purplebuddy", "SetupVersion", plist_new_uint(11));

    plist_t pdata = NULL;

    if (mis) {

        get_cloud_config(&pdata);


        if (pdata && (plist_get_node_type(pdata) == PLIST_DICT)) {
            mcinstall_error_t ret = mcinstall_install_cloud_config(mis, pdata);
            if (MCINSTALL_E_SUCCESS == ret) {
                printf("mcinstall success.\n");
            } else {
                int sc = mcinstall_get_status_code(mis);
                fprintf(stderr, "mcinstall failed, status code: 0x%x\n", sc);
            }
        }
    }
    if (pdata)
        plist_free(pdata);

    cleanup:

    if (mis)
        mcinstall_client_free(mis);

    if (svc)
        lockdownd_service_descriptor_free(svc);

    if (lockdown)
        lockdownd_client_free(lockdown);

    if (device)
        idevice_free(device);

    return result;

}

static void print_usage(int argc, char** argv, int is_error)
{
    char *name = strrchr(argv[0], '/');
    fprintf(is_error ? stderr : stdout, "Usage: %s [OPTIONS] [NAME]\n", (name ? name + 1: argv[0]));
    fprintf(is_error ? stderr : stdout,
            "\n"
            "Display the device name or set it to NAME if specified.\n"
            "\n"
            "OPTIONS:\n"
            "  -u, --udid UDID       target specific device by UDID\n"
            "  -h, --help            print usage information\n"
            "  -v, --version         print version information\n"
            "\n"
            "Homepage:    <" PACKAGE_URL ">\n"
                                         "Bug Reports: <" PACKAGE_BUGREPORT ">\n"
    );
}

int main(int argc, char *argv[]) {
    /*
    ָ��udid
        const char udid[] = "00008101-001C4D321400801E";
        SkipSetup(udid, NULL);

    ����������
        SkipSetup(NULL, NULL);

    ��������
        SkipSetup(NULL, "ahs_mc.plist");
    */

    int c = 0;
    const struct option longopts[] = {
            { "udid",    required_argument, NULL, 'u' },
            { "help",    no_argument,       NULL, 'h' },
            { "version", no_argument,       NULL, 'v' },
            { NULL, 0, NULL, 0}
    };
    const char *udid = NULL;

    while ((c = getopt_long(argc, argv, "u:hv", longopts, NULL)) != -1) {
        switch (c) {
            case 'u':
                if (!*optarg) {
                    fprintf(stderr, "ERROR: UDID must not be empty!\n");
                    print_usage(argc, argv, 1);
                    exit(2);
                }
                udid = optarg;
                break;
            case 'h':
                print_usage(argc, argv, 0);
                return 0;
                break;
            case 'v':
                printf("%s %s\n", TOOL_NAME, PACKAGE_VERSION);
                return 0;
            default:
                print_usage(argc, argv, 1);
                return 2;
        }
    }


    char mcFilename[64] = {0};
//    if (argc == 2) {
//        strcpy(mcFilename, argv[1]);
//    }

    if (EXIT_FAILURE == skip_setup(udid, mcFilename)) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
