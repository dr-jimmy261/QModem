/*
    Copyright 2023 Quectel Wireless Solutions Co.,Ltd

    Quectel hereby grants customers of Quectel a license to use, modify,
    distribute and publish the Software in binary form provided that
    customers shall have no right to reverse engineer, reverse assemble,
    decompile or reduce to source code form any portion of the Software.
    Under no circumstances may customers modify, demonstrate, use, deliver
    or disclose any portion of the Software in source code form.
*/

#include <getopt.h>
#include <grp.h>
#include <sys/types.h>
#include <pwd.h>
#ifdef USE_IPC_MSG
#include <sys/msg.h>
#include <sys/ipc.h>
#endif
#include <inttypes.h>

#include "usb_linux.h"
#include "md5.h"

/*
[PATCH 3.10 27/54] usb: xhci: Add support for URB_ZERO_PACKET to bulk/sg transfers
https://www.spinics.net/lists/kernel/msg2100618.html

commit 4758dcd19a7d9ba9610b38fecb93f65f56f86346
Author: Reyad Attiyat <reyad.attiyat@gmail.com>
Date:   Thu Aug 6 19:23:58 2015 +0300

    usb: xhci: Add support for URB_ZERO_PACKET to bulk/sg transfers

    This commit checks for the URB_ZERO_PACKET flag and creates an extra
    zero-length td if the urb transfer length is a multiple of the endpoint's
    max packet length.
*/
unsigned qusb_zlp_mode = 1; //MT7621 donot support USB ZERO PACKET
unsigned q_erase_all_before_download = 0;
unsigned q_module_packet_sign = 0;
unsigned int g_from_ecm_to_rndis = 0;
const char *q_device_type = "nand"; //nand/emmc/ufs
int sahara_main(const char *firehose_dir, const char *firehose_mbn, void *usb_handle, int edl_mode_05c69008);
int firehose_main (const char *firehose_dir, void *usb_handle, unsigned qusb_zlp_mode);
int stream_download(const char *firehose_dir, void *usb_handle, unsigned qusb_zlp_mode);
int retrieve_soft_revision(void *usb_handle, uint8_t *mobile_software_revision, unsigned length);
int usb2tcp_main(const void *usb_handle, int tcp_port, unsigned qusb_zlp_mode);
int ql_capture_usbmon_log(const char* usbmon_logfile);
void ql_stop_usbmon_log();

//process vals
static long long all_bytes_to_transfer = 0;    //need transfered
static long long transfer_bytes = 0;        //transfered bytes;

int g_is_module_adb_entry_edl = 0;

int g_is2mdn_path = 0;

int switch_to_edl_mode(void *usb_handle) {
    //DIAG commands used to switch the Qualcomm devices to EDL (Emergency download mode)
    unsigned char edl_cmd[] = {0x4b, 0x65, 0x01, 0x00, 0x54, 0x0f, 0x7e};
    //unsigned char edl_cmd[] = {0x3a, 0xa1, 0x6e, 0x7e}; //DL (download mode)
    unsigned char *pbuf = malloc(512);
    if (pbuf == NULL)
    {
        return 0;
    }

    int rx_len;
    int rx_count = 0;

     do {
        rx_len = qusb_noblock_read(usb_handle, pbuf , 512, 0, 1000);
        if (rx_count++ > 100)
            break;
    } while (rx_len > 0);

    dbg_time("switch to 'Emergency download mode'\n");
    rx_len = qusb_noblock_write(usb_handle, edl_cmd, sizeof(edl_cmd), sizeof(edl_cmd), 3000, 0);
    if (rx_len < 0)
        return 0;

    rx_count = 0;

    do {
        rx_len = qusb_noblock_read(usb_handle, pbuf , 512, 0, 3000);
        if (rx_len == sizeof(edl_cmd) && memcmp(pbuf, edl_cmd, sizeof(edl_cmd)) == 0) {
            dbg_time("successful, wait module reboot\n");
            safe_free(pbuf);
            return 1;
        }

        if (rx_count++ > 50)
            break;

    } while (rx_len > 0);

    safe_free(pbuf);
    return 0;
}

int switch_to_edl_mode_in_adb_way()
{
    dbg_time("entry switch_to_edl_mode_in_adb_way \r\n");
    int res = -1;
    res = system("adb shell lxc-power1 adb host");
    // if (res == 127)
    // {
    //     printf("call /bin/sh return error \r\n");
    //     return res;
    // }
    // else if (res == -1)
    // {
    //     printf("just return: error \r\n");
    //     return res;
    // }
    // else if (res == 0)
    // {
    //     printf("no child pid create: error \r\n");
    //     return res;
    // }

    dbg_time("send lxc power1 success res=[%d] \r\n", res);
    sleep(20);
    res = system("adb reboot edl");
    // if (res == 127)
    // {
    //     printf("call /bin/sh return error \r\n");
    //     return res;
    // }
    // else if (res == -1)
    // {
    //     printf("just return: error \r\n");
    //     return res;
    // }
    // else if (res == 0)
    // {
    //     printf("no child pid create: error \r\n");
    //     return res;
    // }

    dbg_time("send reboot edl success res=[%d] \r\n", res);
    return 0;
}

static void usage(int status, const char *program_name)
{
    if(status != EXIT_SUCCESS)
    {
        printf("Try '%s --help' for more information.\n", program_name);
    }
    else
    {
        dbg_time("Upgrade Quectel's modules with Qualcomm's firehose protocol.\n");
        dbg_time("Usage: %s [options...]\n", program_name);
        dbg_time("    -f [package_dir]               Upgrade package directory path\n");
        dbg_time("    -p [/dev/ttyUSBx]              Diagnose port, will auto-detect if not specified\n");
        dbg_time("    -s [/sys/bus/usb/devices/xx]   When multiple modules exist on the board, use -s specify which module you want to upgrade\n");
        dbg_time("    -l [dir_name]                  Sync log into a file(will create qfirehose_timestamp.log)\n");
        dbg_time("    -u [usbmon_log]                Catch usbmon log and save to file (need debugfs and usbmon driver)\n");
        dbg_time("    -n                             Skip MD5 check\n");
        dbg_time("    -d                             Device Type, default nand, support emmc/ufs\n");
        dbg_time("    -v                             For AG215S-GLR signed firmware packages\n");
    }
    exit(status);
}

/*
1. enum dir, fix up dirhose_dir
2. md5 examine
3. furture
*/
static char * find_firehose_mbn(char *firehose_dir, size_t size)
{
    char *firehose_mbn = NULL;

    {
        if (strstr(firehose_dir, "/update/firehose") == NULL) {
            size_t len = strlen(firehose_dir);

            strncat(firehose_dir, "/update/firehose", size);
            if (access(firehose_dir, R_OK)) {
                (firehose_dir)[len] = '\0'; // for smart module
            }
        }

        if (access(firehose_dir, R_OK)) {
            dbg_time("%s access(%s fail), errno: %d (%s)\n", __func__, firehose_dir, errno, strerror(errno));
            return NULL;
        }

        if (!qfile_find_file(firehose_dir, "prog_nand_firehose_", ".mbn", &firehose_mbn)
            && !qfile_find_file(firehose_dir, "prog_emmc_firehose_", ".mbn", &firehose_mbn)
            && !qfile_find_file(firehose_dir, "prog_firehose_", ".mbn", &firehose_mbn)
            && !qfile_find_file(firehose_dir, "prog_firehose_", ".elf", &firehose_mbn)
            && !qfile_find_file(firehose_dir, "firehose-prog", ".mbn", &firehose_mbn)
            && !qfile_find_file(firehose_dir, "prog_", ".mbn", &firehose_mbn)
            && !qfile_find_file(firehose_dir, "xbl_s_devprg_Qcm8550_ns", ".melf", &firehose_mbn) //smart  SA885GAPNA
            && !qfile_find_file(firehose_dir, "xbl_s_devprg_ns_SA52X", ".melf", &firehose_mbn)  //AG590ECNABR01A01M8G_OCPU_01.001.01
            && !qfile_find_file(firehose_dir, "xbl_s_devprg_ns_QCM8538", ".melf", &firehose_mbn)  //AS830MCNAAR01A0224H256_LATC_BPS_BP01.002
          ) {
            dbg_time("%s fail to find firehose mbn file in %s\n", __func__, firehose_dir);
            safe_free(firehose_mbn);
            return NULL;
        }
    }

    dbg_time("%s %s\n", __func__, firehose_mbn);
    return firehose_mbn;
}

#if 0
static int detect_and_judge_module_version(void *usb_handle) {
    static uint8_t version[64] = {'\0'};

    if (usb_handle && version[0] == '\0') {
        retrieve_soft_revision(usb_handle, version, sizeof(version));
        if (version[0]) {
            size_t i = 0;
            size_t length = strlen((const char *)version) - strlen("R00A00");
            dbg_time("old software version: %s\n", version);
            for (i = 0; i < length; i++) {
                if (version[i] == 'R' && isdigit(version[i+1]) &&  isdigit(version[i+2])
                    && version[i+3] == 'A'  && isdigit(version[i+4]) &&  isdigit(version[i+5]))
                {
                    version[i] = '\0';
                    //dbg_time("old hardware version: %s\n", mobile_software_revision);
                    break;
                }
            }
        }
    }

    if (version[0])
        return 0;

    error_return();
}
#endif

struct _firehose_ctx {
    char file_message[MAX_PATH];
    char firehose_dir[MAX_PATH];
    char firehose_mbn[MAX_PATH];
    char module_port_name[MAX_PATH];
    char module_sys_path[MAX_PATH];
    char usbmon_logfile[MAX_PATH];
    char filename[128];
    char q_device_type[16];
};

FILE* loghandler = NULL;
#ifdef FIREHOSE_ENABLE
int firehose_main_entry(int argc, char* argv[])
#else
int main(int argc, char* argv[])
#endif
{
    int opt;
    int check_hash = 1;
    int retval;
    void *usb_handle = NULL;
    int idVendor = 0, idProduct = 0, interfaceNum = 0;
    int edl_retry = 30; //SDX55 require long time by now 20190412
    double start;

    int usb3_speed;
    struct timespec usb3_atime;
    int usb2tcp_port = 0;

    struct _firehose_ctx *ctx = malloc(sizeof(struct _firehose_ctx));
    if (ctx == NULL)
        return -1;
    memset(ctx , 0, sizeof(struct _firehose_ctx));
    ctx->firehose_dir[0] = ctx->module_port_name[0] = ctx->module_sys_path[0] = '\0';

    /* set file priviledge mask 0 */
    umask(0);
    /*build V1.0.8*/
    dbg_time("Version: QFirehose_Linux_Android_V1.7.1\n"); //when release, rename to V1.X
#ifndef __clang__
    dbg_time("Builded: %s %s\n", __DATE__,__TIME__);
#endif

#ifdef ANDROID
    struct passwd* pd;
    pd = getpwuid(getuid());
    dbg_time("------------------\n");
    dbg_time("User:\t %s\n",pd->pw_name);
    struct group* group;
    group = getgrgid(pd->pw_gid);
    dbg_time("Group:\t %s\n", group->gr_name);
    dbg_time("------------------\n");
#if 0 //not all customers need this function
    loghandler = fopen("/data/upgrade.log", "w+");
#endif
    if (loghandler) dbg_time("upgrade log will be sync to /data/upgrade.log\n");
#endif

    optind = 1;
    while ( -1 != (opt = getopt(argc, argv, "f:p:z:s:l:u:d:nevhr"))) {
        switch (opt) {
            case 'n':
                check_hash = 0;
            break;
            case 'l':
                if (loghandler) {
                    fclose(loghandler);
                    loghandler = NULL;
                }
                snprintf(ctx->filename, sizeof(ctx->filename), "%.80s/qfirehose_%"PRIu64".log", optarg, (uint64_t)time(NULL));
                loghandler = fopen(ctx->filename, "w+");
                if (loghandler) dbg_time("upgrade log will be sync to %s\n", ctx->filename);
            break;
            case 'f':
                strncpy(ctx->file_message, optarg, sizeof(ctx->file_message) - 1);
                if (strstr(ctx->file_message, ".mbn") != NULL || strstr(ctx->file_message, ".elf") != NULL)
                {
                    g_is2mdn_path = 1;
                    char *tmp = strrchr(ctx->file_message, '/');
                    strncpy(ctx->firehose_mbn, tmp + 1, strlen(tmp) - 1);
                    strncpy(ctx->firehose_dir, ctx->file_message, strlen(ctx->file_message) - strlen(tmp));
                    dbg_time("f pargram: "
                             "g_is2mdn_path=[%d],file_message=[%s],firehose_mbn=[%s],"
                             "firehose_dir=[%s]\n",
                             g_is2mdn_path, ctx->file_message, ctx->firehose_mbn, ctx->firehose_dir);
                    break;
                }
                strncpy(ctx->firehose_dir, ctx->file_message, strlen(ctx->file_message));
            break;
            case 'p':
                strncpy(ctx->module_port_name, optarg, sizeof(ctx->module_port_name) - 1);
                if (!strcmp(ctx->module_port_name, "9008")) {
                    usb2tcp_port = atoi(ctx->module_port_name);
                    ctx->module_port_name[0] = '\0';
                }
            break;
            case 's':
                strncpy(ctx->module_sys_path, optarg, sizeof(ctx->module_sys_path) - 1);
                if (ctx->module_sys_path[strlen(ctx->module_sys_path)-1] == '/')
                    ctx->module_sys_path[strlen(ctx->module_sys_path)-1] = '\0';
            break;
            case 'z':
                qusb_zlp_mode = !!atoi(optarg);
            break;
            case 'e':
                q_erase_all_before_download = 1;
            break;
            case 'u':
                strncpy(ctx->usbmon_logfile, optarg, sizeof(ctx->usbmon_logfile) - 1);
            break;
            case 'd':
                strncpy(ctx->q_device_type, optarg, sizeof(ctx->q_device_type) - 1);
                q_device_type = ctx->q_device_type;
            break;
            case 'v':
                q_module_packet_sign = 1;
            break;
            case 'r':
                g_from_ecm_to_rndis = 1;
                dbg_time("will use rndis mode [%d]\r\n", g_from_ecm_to_rndis);
            break;
            case 'h':
                usage(EXIT_SUCCESS, argv[0]);
            break;
            default:
            break;
        }
    }

    if (ctx->usbmon_logfile[0])
        ql_capture_usbmon_log(ctx->usbmon_logfile);

    update_transfer_bytes(0);
    if (usb2tcp_port)
        goto _usb2tcp_start;

    if (ctx->firehose_dir[0] == '\0') {
        usage(EXIT_SUCCESS, argv[0]);
        retval = -1;
        goto _firehose_exit;
    }

    if (access(ctx->firehose_dir, R_OK)) {
        dbg_time("fail to access %s, errno: %d (%s)\n", ctx->firehose_dir, errno, strerror(errno));
        retval = -1;
        goto _firehose_exit;
    }

    opt = strlen(ctx->firehose_dir);
    if (ctx->firehose_dir[opt-1] == '/') {
        ctx->firehose_dir[opt-1] = '\0';
    }

    if (strEndsWith(ctx->firehose_dir, ".7z") || strEndsWith(ctx->firehose_dir, ".zip")) {
        char tmp_dir[32];

        snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/qfirehose-unzip-%d", getpid());
        if (unzip_to_tmp_dir(ctx->firehose_dir, tmp_dir) != 0) {
            dbg_time("fail to unzip to %s, errno: %d (%s)\n", tmp_dir, errno, strerror(errno));
            retval = -2;
            goto _firehose_exit;
        }
        strcpy(ctx->firehose_dir, tmp_dir);
    }

    if (check_hash && md5_check(ctx->firehose_dir)) {
        retval = -3;
        goto _firehose_exit;
    }

    char *firehose_mbn = NULL;
    if (!g_is2mdn_path) 
    {
        firehose_mbn = find_firehose_mbn(ctx->firehose_dir, sizeof(ctx->firehose_dir));
        dbg_time("%s %s\n", __func__, firehose_mbn);
    }

    if (!firehose_mbn) {
        retval = -4;
        goto _firehose_exit;
    }
    strncpy(ctx->firehose_mbn, firehose_mbn, sizeof(ctx->firehose_mbn));
    safe_free(firehose_mbn);

    if (ctx->module_port_name[0] && !strncmp(ctx->module_port_name, "/dev/mhi", strlen("/dev/mhi"))) {
        if (qpcie_open(ctx->firehose_dir, ctx->firehose_mbn, ctx->module_port_name)) {
            retval = -5;
            goto _firehose_exit;
        }

        usb_handle = &edl_pcie_mhifd;
        start = get_now();
        goto __firehose_main;
    }
    else if (ctx->module_port_name[0] && strstr(ctx->module_port_name, ":9008")) {
        strcpy(ctx->module_sys_path, ctx->module_port_name);
        goto __edl_retry;
    }

_usb2tcp_start:
    if (ctx->module_sys_path[0] && access(ctx->module_sys_path, R_OK)) {
        dbg_time("fail to access %s, errno: %d (%s)\n", ctx->module_sys_path, errno, strerror(errno));
        retval = -6;
        goto _firehose_exit;
    }

    if (ctx->module_port_name[0] && access(ctx->module_port_name, R_OK | W_OK)) {
        dbg_time("fail to access %s, errno: %d (%s)\n", ctx->module_port_name, errno, strerror(errno));
        retval = -7;
        goto _firehose_exit;
    }

    if (ctx->module_sys_path[0] == '\0' && ctx->module_port_name[0] != '\0') {
        //get sys path by port name
        quectel_get_syspath_name_by_ttyport(ctx->module_port_name, ctx->module_sys_path, MAX_PATH);
    }

    g_is_module_adb_entry_edl = 0;

    if (ctx->module_sys_path[0] == '\0') {
        int module_count = auto_find_quectel_modules(ctx->module_sys_path, MAX_PATH, NULL, NULL);
        if (module_count <= 0) {
            dbg_time("Quectel module not found\n");
            retval = -8;
            goto _firehose_exit;
        }
        else if (module_count == 1) {
            if (g_is_module_adb_entry_edl > 0)
            {
                switch_to_edl_mode_in_adb_way();
            }
        } else {
            dbg_time("There are multiple quectel modules in system, Please use <-s /sys/bus/usb/devices/xx> specify which module you want to upgrade!\n");
            dbg_time("The module's </sys/bus/usb/devices/xx> path was printed in the previous log!\n");
            retval = -9;
            goto _firehose_exit;
        }
    }

__edl_retry:
    qusb_read_speed_atime(ctx->module_sys_path, &usb3_atime, &usb3_speed);
    while (edl_retry-- > 0) {
        usb_handle = qusb_noblock_open(ctx->module_sys_path, &idVendor, &idProduct, &interfaceNum);

        if (usb_handle) {
            clock_gettime(CLOCK_REALTIME, &usb3_atime);
        }
        else {
            sleep(1); //in reset sate, wait connect
            if (usb3_speed >= 5000 && access(ctx->module_sys_path, R_OK) && errno_nodev()) {
                if (auto_find_quectel_modules(ctx->module_sys_path, MAX_PATH, "5c6/9008/", &usb3_atime) > 1) {
                    dbg_time("There are multiple quectel EDL modules in system!\n");
                    retval = -10;
                    goto _firehose_exit;
                }
            }
            continue;
        }

#if 0
        if (idVendor == 0x2c7c && interfaceNum > 1) {
            if (detect_and_judge_module_version(usb_handle)) {
                // update_transfer_bytes(-1);
                /* do not return here, this command will fail when modem is not ready */
                // error_return();
            }
        }
#endif

        if (interfaceNum == 1) {
            if ((idVendor == 0x2C7C) && (idProduct == 0x0800)) {
                // although 5G module stay in dump mode, after send edl command, it also can enter edl mode
                dbg_time("5G module stay in dump mode!\n");
            } else {
                break;
            }
            dbg_time("something went wrong???, why only one interface left\n");
        }

        switch_to_edl_mode(usb_handle);
        qusb_noblock_close(usb_handle);
        usb_handle = NULL;
        sleep(1); //wait usb disconnect and re-connect
    }

    if (usb_handle == NULL) {
        retval = -11;
        goto _firehose_exit;
    }

    if (usb2tcp_port) {
        retval = usb2tcp_main(usb_handle, usb2tcp_port, qusb_zlp_mode);
        goto _firehose_exit;
    }

    start = get_now();
    retval = sahara_main(ctx->firehose_dir, ctx->firehose_mbn, usb_handle, idVendor == 0x05c6);

    if (!retval) {
        if (idVendor != 0x05C6) {
            sleep(1);
            stream_download(ctx->firehose_dir, usb_handle, qusb_zlp_mode);
            qusb_noblock_close(usb_handle);
            sleep(10);      //EM05-G switching to download mode is slow and increases the waiting time to 10 seconds
            goto __edl_retry;
        }

__firehose_main:
        retval = firehose_main(ctx->firehose_dir, usb_handle, qusb_zlp_mode);
        if(retval == 0)
        {
            get_duration(start);
        }
    }

_firehose_exit:
    if (usb_handle) qusb_noblock_close(usb_handle);
    if (loghandler) fclose(loghandler);
    if (ctx->usbmon_logfile[0]) ql_stop_usbmon_log();
    if (retval) update_transfer_bytes(-1);
    unzip_rm_tmp_dir();
    if (ctx) safe_free(ctx);
    dbg_time("Upgrade module %s.\n", retval == 0 ? "successfully" : "failed");

    return retval;
}

double get_now()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000;
}

void get_duration(double start)
{
    dbg_time("THE TOTAL DOWNLOAD TIME IS %.3f s\n",(get_now() - start));
}

void set_transfer_allbytes(long long bytes)
{
    transfer_bytes = 0;
    all_bytes_to_transfer = bytes;
}

int update_progress_msg(int percent);
int update_progress_file(int percent);
/*
return percent
*/
int update_transfer_bytes(long long bytes_cur)
{
    static int last_percent = -1;
    int percent = 0;

    if (bytes_cur == -1 || bytes_cur == 0)
    {
        percent = bytes_cur;
    }
    else
    {
        transfer_bytes += bytes_cur;
        percent = (transfer_bytes * 100) / all_bytes_to_transfer;
    }

    if (percent != last_percent)
    {
        last_percent = percent;
#ifdef USE_IPC_FILE
        update_progress_file(percent);
#endif
#ifdef USE_IPC_MSG
        update_progress_msg(percent);
#endif
    }

    return percent;
}

void show_progress()
{
    static int percent = 0;

    if (all_bytes_to_transfer)
        percent = (transfer_bytes * 100) / all_bytes_to_transfer;
    dbg_time("upgrade progress %d%% %lld/%lld\n", percent, transfer_bytes, all_bytes_to_transfer);
}

#ifdef USE_IPC_FILE
#define IPC_FILE_ANDROID "/data/update.conf"
#define IPC_FILE_LINUX "/tmp/update.conf"
int update_progress_file(int percent)
{
    static int ipcfd = -1;
    char buff[16];

    if (ipcfd < 0)
    {
#ifdef ANDROID
        const char *ipc_file = IPC_FILE_ANDROID;
#else
        const char *ipc_file = IPC_FILE_LINUX;
#endif
        /* Have set umask previous, no need to call fchmod */
        ipcfd = open(ipc_file, O_TRUNC | O_CREAT | O_WRONLY | O_NONBLOCK, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
        if (ipcfd < 0)
        {
            dbg_time("Fail to open(O_WRONLY) %s: %s\n", ipc_file, strerror(errno));
            return -1;
        }
    }

    lseek(ipcfd, 0, SEEK_SET);
    snprintf(buff, sizeof(buff), "%d", percent);
    if (write(ipcfd, buff, strlen(buff)) < 0)
        dbg_time("fail to write upgrade progress into %s: %s\n", ipc_file, strerror(errno));

    if (percent == 100 || percent < 0)
        close(ipcfd);
    return 0;
}
#endif

#ifdef USE_IPC_MSG
#define MSGBUFFSZ 16
struct message
{
    long mtype;
    char mtext[MSGBUFFSZ];
};

#define MSG_FILE "/etc/passwd"
#define MSG_TYPE_IPC 1
static int msg_get()
{
    key_t key = ftok(MSG_FILE, 'a');
    int msgid = msgget(key, IPC_CREAT | 0644);

    if (msgid < 0)
    {
        dbg_time("msgget fail: key %d, %s\n", key, strerror(errno));
        return -1;
    }
    return msgid;
}

static int msg_rm(int msgid)
{
    return msgctl(msgid, IPC_RMID, 0);
}

static int msg_send(int msgid, long type, const char *msg)
{
    struct message info;
    info.mtype = type;
    snprintf(info.mtext, MSGBUFFSZ, "%s", msg);
    if (msgsnd(msgid, (void *)&info, MSGBUFFSZ, IPC_NOWAIT) < 0)
    {
        dbg_time("msgsnd faild: msg %s, %s\n", msg, strerror(errno));
        return -1;
    }
    return 0;
}

static int msg_recv(int msgid, struct message *info)
{
    if (msgrcv(msgid, (void *)info, MSGBUFFSZ, info->mtype, IPC_NOWAIT) < 0)
    {
        dbg_time("msgrcv faild: type %ld, %s\n", info->mtype, strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * this function will not delete the msg queue
 */
int update_progress_msg(int percent)
{
    char buff[MSGBUFFSZ];
    int msgid = msg_get();
    if (msgid < 0)
        return -1;
    snprintf(buff, sizeof(buff), "%d", percent);

#ifndef IPC_TEST
    return msg_send(msgid, MSG_TYPE_IPC, buff);
#else
    msg_send(msgid, MSG_TYPE_IPC, buff);
    struct message info;
    info.mtype = MSG_TYPE_IPC;
    msg_recv(msgid, &info);
    printf("msg queue read: %s\n", info.mtext);
#endif
}
#endif
