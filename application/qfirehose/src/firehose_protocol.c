/*
    Copyright 2023 Quectel Wireless Solutions Co.,Ltd

    Quectel hereby grants customers of Quectel a license to use, modify,
    distribute and publish the Software in binary form provided that
    customers shall have no right to reverse engineer, reverse assemble,
    decompile or reduce to source code form any portion of the Software.
    Under no circumstances may customers modify, demonstrate, use, deliver
    or disclose any portion of the Software in source code form.
*/

#include "usb_linux.h"
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>

#define error_return()  do {dbg_time("%s %s %d fail\n", __FILE__, __func__, __LINE__); return __LINE__; } while(0)
int recv_sc600y_configure_num = 1;
extern const char *q_device_type;
static int fh_recv_cmd_sk[2];
extern unsigned q_module_packet_sign;

extern unsigned q_erase_all_before_download;
extern int update_transfer_bytes(long long bytes_cur);
extern int show_progress();

char file_name_image[128] = {0};
char file_name_image_dir[256] = {0};

typedef struct sparse_header
{
  uint32_t magic;         /* 0xed26ff3a */
  uint16_t major_version; /* (0x1) - reject images with higher major versions */
  uint16_t minor_version; /* (0x0) - allow images with higer minor versions */
  uint16_t file_hdr_sz;   /* 28 bytes for first revision of the file format */
  uint16_t chunk_hdr_sz;  /* 12 bytes for first revision of the file format */
  uint32_t blk_sz;       /* block size in bytes, must be a multiple of 4 (4096) */
  uint32_t total_blks;   /* total blocks in the non-sparse output image */
  uint32_t total_chunks; /* total chunks in the sparse input image */
  uint32_t image_checksum; /* CRC32 checksum of the original data, counting "don't care" */
  /* as 0. Standard 802.3 polynomial, use a Public Domain */
  /* table implementation */
} sparse_header_t;

#define SPARSE_HEADER_MAGIC 0xed26ff3a
#define SPARSE_CHUNK_TYPE_RAW 0xCAC1  
#define SPARSE_CHUNK_TYPE_FILL 0xCAC2 
#define SPARSE_CHUNK_TYPE_ZERO 0xCAC3 

typedef struct chunk_header
{
  uint16_t chunk_type; /* 0xCAC1 -> raw; 0xCAC2 -> fill; 0xCAC3 -> don't care */
  uint16_t reserved1;
  uint32_t chunk_sz; /* in blocks in output image */
  uint32_t total_sz; /* in bytes of chunk input file including chunk header and data */
} chunk_header_t;

#define SPARSE_CHUNK_MERGE_NONE 10
#define SPARSE_CHUNK_MERGE_RAW_FILL 11
#define SPARSE_CHUNK_MERGE_RAW_FILL_ZERO 12
#define SPARSE_CHUNK_MERGE_POLICY SPARSE_CHUNK_MERGE_RAW_FILL

typedef struct chunk_polymerization_params {
  uint32_t total_chunk_sz;
  uint32_t total_chunk_count;
  uint32_t start_chunk_offset;
  long total_sz;
  long file_sector_offset;
}chunk_polymerization_param;

typedef struct SparseImgParams {
  //chunk_polymerization_param chunk_polymerization_cac3[100];
  uint32_t  total_count;
  //uint16_t  total_cac3_count;
  //uint16_t file_first_sector_offset;    //第一个是头获取，后面都可以计算出来
  uint32_t blk_sz;
  chunk_polymerization_param chunk_polymerization_data[];
} SparseImgParam;

static SparseImgParam *SparseImgData;

struct fh_configure_cmd {
    const char *type;
    const char *MemoryName;
    uint32_t Verbose;
    uint32_t AlwaysValidate;
    uint32_t MaxDigestTableSizeInBytes;
    uint32_t MaxPayloadSizeToTargetInBytes;
    uint32_t MaxPayloadSizeFromTargetInBytes ; 			//2048
    uint32_t MaxPayloadSizeToTargetInByteSupported;		//16k
    uint32_t ZlpAwareHost;
    uint32_t SkipStorageInit;
};

struct fh_erase_cmd {
    const char *type;
    //uint32_t PAGES_PER_BLOCK;
    uint32_t SECTOR_SIZE_IN_BYTES;
    //char label[32];
    uint32_t last_sector;
    uint32_t num_partition_sectors;
    //uint32_t physical_partition_number;
    uint32_t start_sector;
};

struct fh_program_cmd {
    const char *type;
    char *filename;
    char *sparse;
    uint32_t filesz;
    //uint32_t PAGES_PER_BLOCK;
    uint32_t SECTOR_SIZE_IN_BYTES;
    //char label[32];
    //uint32_t last_sector;
    uint32_t num_partition_sectors;
    uint32_t physical_partition_number;
    uint32_t start_sector;
    uint32_t file_sector_offset;
    uint32_t UNSPARSE_FILE_SIZE;
    //char sparse[16];
};

struct fh_response_cmd {
    const char *type;
    const char *value;
    uint32_t rawmode;
    uint32_t MaxPayloadSizeToTargetInBytes;
};

struct fh_log_cmd {
    const char *type;
};

struct fh_patch_cmd {
    const char *type;
    char *filename;
    uint32_t filesz;
    uint32_t SECTOR_SIZE_IN_BYTES;
    uint32_t num_partition_sectors;
};

struct fh_cmd_header {
    const char *type;
};

struct fh_vendor_defines {
    const char *type; // "vendor"
};

struct fh_cmd {
    union {
        struct fh_cmd_header cmd;
        struct fh_configure_cmd cfg;
        struct fh_erase_cmd erase;
        struct fh_program_cmd program;
        struct fh_response_cmd response;
        struct fh_log_cmd log;
        struct fh_patch_cmd patch;
        struct fh_vendor_defines vdef;
    };
    int part_upgrade;
    char xml_original_data[512];
};

#define fh_cmd_num 1024 //AG525 have more than 64 partition
struct fh_data {
    const char *firehose_dir;
    const void *usb_handle;
    unsigned MaxPayloadSizeToTargetInBytes;
    unsigned fh_cmd_count;
    unsigned fh_patch_count;
    unsigned ZlpAwareHost;
    struct fh_cmd fh_cmd_table[fh_cmd_num]; 

    unsigned xml_tx_size;
    unsigned xml_rx_size;
    char xml_tx_buf[1024];
    char xml_rx_buf[1024];
};

static const char * fh_xml_find_value(const char *xml_line, const char *key, char **ppend) {
    char *pchar = strstr(xml_line, key);
    char *pend;

    if (!pchar) {
        if (strcmp(key, "sparse"))
            dbg_time("%s: no key %s in %s\n", __func__, key, xml_line);
        return NULL;
    }

    pchar += strlen(key);
    if (pchar[0] != '=' && pchar[1] != '"') {
        dbg_time("%s: no start %s in %s\n", __func__, "=\"", xml_line);
        return NULL;
    }

    pchar += strlen("=\"");
    pend = strstr(pchar, "\"");
    if (!pend) {
        dbg_time("%s: no end %s in %s\n", __func__, "\"", xml_line);
        return NULL;
    }

    *ppend = pend;
    return pchar;
}

static const char * fh_xml_get_value(const char *xml_line, const char *key) {
    static char value[64];
    char *pend;
    const char *pchar = fh_xml_find_value(xml_line, key, &pend);

    if (!pchar) {
        return NULL;
    }

    int len = pend - pchar;
    if (len >= 64)
        return NULL;

    strncpy(value, pchar, pend - pchar);
    value[pend - pchar] = '\0';

    return value;
}

static void fh_xml_set_value(char *xml_line, const char *key, unsigned value) {
    char *pend;
    const char *pchar = fh_xml_find_value(xml_line, key, &pend);
    char value_str[32];
     char *tmp_line = malloc(strlen(xml_line) + 1 + sizeof(value_str));

    if (!pchar || !tmp_line) {

        if (tmp_line)
        {
            free(tmp_line);
            tmp_line = NULL;
        }
        return;
    }

    strcpy(tmp_line, xml_line);

    snprintf(value_str, sizeof(value_str), "%u", value);
    tmp_line[pchar - xml_line] = '\0';
    strcat(tmp_line, value_str);
    strcat(tmp_line, pend);

    strcpy(xml_line, tmp_line);
    free(tmp_line);
}

static int fh_parse_xml_line(const char *xml_line, struct fh_cmd *fh_cmd) {
    const char *pchar = NULL;
    size_t len = strlen(xml_line);

    memset(fh_cmd, 0, sizeof( struct fh_cmd));
    strncpy(fh_cmd->xml_original_data, xml_line, 512);
    if (fh_cmd->xml_original_data[len - 1] == '\n')
        fh_cmd->xml_original_data[len - 1] = '\0';

    if (strstr(xml_line, "vendor=\"quectel\"")) {
        fh_cmd->vdef.type = "vendor";
        return 0;
    }
    else if (!strncmp(xml_line, "<erase ", strlen("<erase "))) {
        fh_cmd->erase.type = "erase";
        if (strstr(xml_line, "last_sector")) {
            if ((pchar = fh_xml_get_value(xml_line, "last_sector")))
                fh_cmd->erase.last_sector = atoi(pchar);
        }
        if ((pchar = fh_xml_get_value(xml_line, "start_sector")))
            fh_cmd->erase.start_sector = atoi(pchar);
        if ((pchar = fh_xml_get_value(xml_line, "num_partition_sectors")))
            fh_cmd->erase.num_partition_sectors = atoi(pchar);
        if ((pchar = fh_xml_get_value(xml_line, "SECTOR_SIZE_IN_BYTES")))
            fh_cmd->erase.SECTOR_SIZE_IN_BYTES = atoi(pchar);

        return 0;
    }
    else if (!strncmp(xml_line, "<program ", strlen("<program "))) {
        fh_cmd->program.type = "program";
        if ((pchar = fh_xml_get_value(xml_line, "filename")))
        {
            fh_cmd->program.filename = strdup(pchar);
            if(fh_cmd->program.filename[0] == '\0')
            {//some fw version have blank program line, ignore it.
                return -1;
            }
        }

        if ((pchar = fh_xml_get_value(xml_line, "sparse")))
        {
            fh_cmd->program.sparse = strdup(pchar);
        }
        else
            fh_cmd->program.sparse = NULL;

        if ((pchar = fh_xml_get_value(xml_line, "start_sector")))
            fh_cmd->program.start_sector = atoi(pchar);
        if ((pchar = fh_xml_get_value(xml_line, "num_partition_sectors")))
            fh_cmd->program.num_partition_sectors = atoi(pchar);
        if ((pchar = fh_xml_get_value(xml_line, "SECTOR_SIZE_IN_BYTES")))
            fh_cmd->program.SECTOR_SIZE_IN_BYTES = atoi(pchar);

        if (fh_cmd->program.sparse != NULL && !strncasecmp(fh_cmd->program.sparse, "true", 4))
        {
            if ((pchar = fh_xml_get_value(xml_line, "file_sector_offset")))
                fh_cmd->program.file_sector_offset = atoi(pchar);
            if ((pchar = fh_xml_get_value(xml_line, "physical_partition_number")))
                fh_cmd->program.physical_partition_number = atoi(pchar);
        }

        return 0;
    }
    else if (!strncmp(xml_line, "<patch ", strlen("<patch "))) {
        fh_cmd->patch.type = "patch";
        pchar = fh_xml_get_value(xml_line, "filename");
        if (pchar && strcmp(pchar, "DISK"))
            return -1;
        return 0;
    }
    else if (!strncmp(xml_line, "<response ", strlen("<response "))) {
        fh_cmd->response.type = "response";
        pchar = fh_xml_get_value(xml_line, "value");
        if (pchar) {
            if (!strcmp(pchar, "ACK"))
                fh_cmd->response.value =  "ACK";
            else if(!strcmp(pchar, "NAK"))
                fh_cmd->response.value =  "NAK";
            else
                 fh_cmd->response.value =  "OTHER";
        }
        if (strstr(xml_line, "rawmode")) {
            pchar = fh_xml_get_value(xml_line, "rawmode");
            if (pchar) {
                fh_cmd->response.rawmode = !strcmp(pchar, "true");
            }
        }
        else if (strstr(xml_line, "MaxPayloadSizeToTargetInBytes")) {
            pchar = fh_xml_get_value(xml_line, "MaxPayloadSizeToTargetInBytes");
            if (pchar) {
                fh_cmd->response.MaxPayloadSizeToTargetInBytes = atoi(pchar);
            }
        }
        return 0;
    }
    else if (!strncmp(xml_line, "<log ", strlen("<log "))) {
        fh_cmd->program.type = "log";
        return 0;
    }

    error_return();
}

static int fh_parse_xml_file(struct fh_data *fh_data, const char *xml_file) {
    FILE *fp = unzip_fopen(xml_file, "rb");

    if (fp == NULL) {
        dbg_time("%s fail to fopen(%s), errno: %d (%s)\n", __func__, xml_file, errno, strerror(errno));
        error_return();
    }

    while (fgets(fh_data->xml_tx_buf, fh_data->xml_tx_size, fp)) {
        char *xml_line = strstr(fh_data->xml_tx_buf, "<");
        char *c_start = NULL;

        if (!xml_line)
            continue;

        c_start = strstr(xml_line, "<!--");
        if (c_start) {
            char *c_end = strstr(c_start, "-->");

            if (c_end) {
                /*
                <erase case 1 /> <!-- xxx -->
                <!-- xxx --> <erase case 2 />
                <!-- <erase case 3 /> -->
                */
                char *tmp = strstr(xml_line, "/>");
                if (tmp && (tmp < c_start || tmp > c_end)) {
                    memset(c_start, ' ', c_end - c_start + strlen("-->"));
                    goto __fh_parse_xml_line;
                }

                continue;
            }
            else {
                /*
                     <!-- line1
                             <! -- line2 -->
                      -->
                */
                do {
                    if (fgets(fh_data->xml_tx_buf, fh_data->xml_tx_size, fp) == NULL) { break; };
                    xml_line = fh_data->xml_tx_buf;
                } while (!strstr(xml_line, "-->") && strstr(xml_line, "<!--"));

                continue;
            }
        }

__fh_parse_xml_line:
        if (xml_line) {
            char *tag = NULL;

            tag = strstr(xml_line, "<erase ");
            if (!tag) {
                tag = strstr(xml_line, "<program ");
                if (!tag) {
                    tag = strstr(xml_line, "<patch ");
                }
            }

            if (tag) {
                if (!fh_parse_xml_line(tag, &fh_data->fh_cmd_table[fh_data->fh_cmd_count])) {
                    fh_data->fh_cmd_count++;
                    if (strstr(tag, "<patch "))
                        fh_data->fh_patch_count++;
                    if (fh_data->fh_cmd_count >= fh_cmd_num) {
                        dbg_time("too many fh_cmd, you need to increase fh_cmd_num\n");
                        exit(-1);
                    }
                }
            }
            else if (!strstr(xml_line, "<?xml") && !strcmp(xml_line, "<data>") && !strcmp(xml_line, "</data>")
                && !strcmp(xml_line, "<patches>") && !strcmp(xml_line, "<patches>")) {
                dbg_time("unspport xml_line '%s'\n", xml_line);
                exit(-1);
            }
        }
    }

    unzip_fclose(fp);

    return 0;
}

static int fh_fixup_program_cmd(struct fh_data *fh_data, struct fh_cmd *fh_cmd, long* filesize_out) {
    char full_path[512] = {0};
    char *ptmp;
    uint32_t num_partition_sectors = fh_cmd->program.num_partition_sectors;

    snprintf(full_path, sizeof(full_path), "%.255s/%.240s", fh_data->firehose_dir, fh_cmd->program.filename);
    while((ptmp = strchr(full_path, '\\'))) {
        *ptmp = '/';
    }

    *filesize_out = unzip_get_file_size(full_path);
    if (*filesize_out <= 0) {
        dbg_time("fail to ftell %s, errno: %d (%s)\n", full_path, errno, strerror(errno));
        fh_cmd->program.num_partition_sectors = 0;
        fh_cmd->program.filesz = 0;
        error_return();
    }
    fh_cmd->program.filesz = *filesize_out;

    fh_cmd->program.num_partition_sectors = *filesize_out/fh_cmd->program.SECTOR_SIZE_IN_BYTES;
    if ((*filesize_out)%fh_cmd->program.SECTOR_SIZE_IN_BYTES)
        fh_cmd->program.num_partition_sectors += 1;

    if (strEndsWith(full_path, "gpt_empty0.bin"))
    {
        fh_cmd->program.num_partition_sectors -= 1;
    }

    if (num_partition_sectors != fh_cmd->program.num_partition_sectors) {
        fh_xml_set_value(fh_cmd->xml_original_data, "num_partition_sectors",
            fh_cmd->program.num_partition_sectors);
    }

    return 0;
}

static int _fh_recv_cmd(struct fh_data *fh_data, struct fh_cmd *fh_cmd, unsigned timeout) {
    int ret;
    char *xml_line;
    char *pend;

    memset(fh_cmd, 0, sizeof(struct fh_cmd));

    ret = qusb_noblock_read(fh_data->usb_handle, fh_data->xml_rx_buf, fh_data->xml_rx_size, 1, timeout);
    if (ret <= 0) {
        return -1;
    }
    fh_data->xml_rx_buf[ret] = '\0';

   xml_line = fh_data->xml_rx_buf;
    while (*xml_line) {
        xml_line = strstr(xml_line, "<?xml version=");
        if (xml_line == NULL) {
            if (fh_cmd->cmd.type == 0) {
                dbg_time("{{{%s}}}", fh_data->xml_rx_buf);
                error_return();
            } else {
                break;
            }
        }
        xml_line += strlen("<?xml version=");

        xml_line = strstr(xml_line, "<data>");
        if (xml_line == NULL) {
            dbg_time("{{{%s}}}", fh_data->xml_rx_buf);
            error_return();
        }
        xml_line += strlen("<data>");
        if (xml_line[0] == '\n')
            xml_line++;

        if (!strncmp(xml_line, "<response ", strlen("<response "))) {
            fh_parse_xml_line(xml_line, fh_cmd);
            pend = strstr(xml_line, "/>");
            pend += 2;
            dbg_time("%.*s\n", (int)(pend -xml_line),  xml_line);
            xml_line = pend + 1;
        }
        else if (!strncmp(xml_line, "<log ", strlen("<log "))) {
            if (fh_cmd->cmd.type && strcmp(fh_cmd->cmd.type, "log")) {
                dbg_time("{{{%s}}}", fh_data->xml_rx_buf);
                break;
            }
            fh_parse_xml_line(xml_line, fh_cmd);
            pend = strstr(xml_line, "/>");
            pend += 2;
            {
                char *prn = xml_line;
                while (prn < pend) {
                    if (*prn == '\r' || *prn == '\n')
                        *prn = '.';
                    prn++;
                }
            }
            dbg_time("%.*s\n", (int)(pend -xml_line),  xml_line);
            xml_line = pend + 1;
        } else {
            dbg_time("unkonw %s", xml_line);
            error_return();
        }
    }

    if (fh_cmd->cmd.type)
        return 0;

    error_return();
}

static void * fh_recv_cmd_thread(void *arg) {
    struct fh_data *fh_data = (struct fh_data *)arg;
    struct fh_cmd fh_rx_cmd;

    while (_fh_recv_cmd(fh_data, &fh_rx_cmd, -1) == 0) {
        if (strncmp(fh_rx_cmd.cmd.type, "log", strlen("log")))
        {
            if (write(fh_recv_cmd_sk[1], &fh_rx_cmd, sizeof(fh_rx_cmd)) == -1) { };
        }
    }

    return NULL;
}

static int fh_recv_cmd(struct fh_data *fh_data, struct fh_cmd *fh_cmd, unsigned timeout, int ignore_timeout) {
    struct pollfd pollfds[] = {{fh_recv_cmd_sk[0], POLLIN, 0}};
    int ret = poll(pollfds, 1, timeout);

    (void)fh_data;
    if (ret == 1 && (pollfds[0].revents & POLLIN)) {
        ret = read(fh_recv_cmd_sk[0], fh_cmd, sizeof(struct fh_cmd));
        if (ret == sizeof(struct fh_cmd))
            return 0;
    }
    else if (ret == 0 && ignore_timeout) {
        return __LINE__;
    }

    error_return();
}

static int fh_wait_response_cmd(struct fh_data *fh_data, struct fh_cmd *fh_cmd, unsigned timeout) {
    while (1) {
        int ret = fh_recv_cmd(fh_data, fh_cmd, timeout, 0);

        if (ret !=0)
            error_return();

        if (strstr(fh_cmd->cmd.type, "log"))
            continue;

        return 0;
    }

    error_return();
}

static int fh_send_cmd(struct fh_data *fh_data, const struct fh_cmd *fh_cmd) {
    int tx_len = 0;
    char *pstart, *pend;
    char *xml_buf = fh_data->xml_tx_buf;
    unsigned xml_size = fh_data->xml_tx_size;
    xml_buf[0] = '\0';

    snprintf(xml_buf + strlen(xml_buf), xml_size, "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n");
    snprintf(xml_buf + strlen(xml_buf), xml_size, "<data>\n");

    pstart = xml_buf + strlen(xml_buf);
    if (!strcmp(fh_cmd->cmd.type, "vendor")) {
        snprintf(xml_buf + strlen(xml_buf), xml_size, "%s", fh_cmd->xml_original_data);
    }
    else if (!strcmp(fh_cmd->cmd.type, "erase")) {
        snprintf(xml_buf + strlen(xml_buf), xml_size, "%s", fh_cmd->xml_original_data);
    }
    else if (!strcmp(fh_cmd->cmd.type, "program")) {
        if (fh_cmd->program.sparse != NULL && !strncasecmp(fh_cmd->program.sparse, "true", 4))
        {
            snprintf(xml_buf + strlen(xml_buf), xml_size,
                "<program filename=\"%.120s\" SECTOR_SIZE_IN_BYTES=\"%d\" num_partition_sectors=\"%d\" physical_partition_number=\"%d\" start_sector=\"%d\" file_sector_offset=\"%d\" sparse=\"%.120s\" UNSPARSE_FILE_SIZE=\"%d\" />",
                fh_cmd->program.filename, fh_cmd->program.SECTOR_SIZE_IN_BYTES, fh_cmd->program.num_partition_sectors, fh_cmd->program.physical_partition_number, fh_cmd->program.start_sector, fh_cmd->program.file_sector_offset, fh_cmd->program.sparse, fh_cmd->program.UNSPARSE_FILE_SIZE);
        }
        else
            snprintf(xml_buf + strlen(xml_buf), xml_size, "%s", fh_cmd->xml_original_data);
    }
    else if (!strcmp(fh_cmd->cmd.type, "patch")) {
        snprintf(xml_buf + strlen(xml_buf), xml_size, "%s", fh_cmd->xml_original_data);
    }
    else if (!strcmp(fh_cmd->cmd.type, "configure")) {
        snprintf(xml_buf + strlen(xml_buf), xml_size,
            "<configure MemoryName=\"%.8s\" Verbose=\"%d\" AlwaysValidate=\"%d\" MaxDigestTableSizeInBytes=\"%d\" MaxPayloadSizeToTargetInBytes=\"%d\"  ZlpAwareHost=\"%d\" SkipStorageInit=\"%d\" />",
            fh_cmd->cfg.MemoryName, fh_cmd->cfg.Verbose, fh_cmd->cfg.AlwaysValidate,
            fh_cmd->cfg.MaxDigestTableSizeInBytes,
            fh_cmd->cfg.MaxPayloadSizeToTargetInBytes,
            fh_cmd->cfg.ZlpAwareHost, fh_cmd->cfg.SkipStorageInit);
    }
    else if (!strcmp(fh_cmd->cmd.type, "setbootablestoragedrive")) {
        snprintf(xml_buf + strlen(xml_buf), xml_size, "<setbootablestoragedrive value=\"%d\" />",
            !strcmp(q_device_type, "ufs") ? 1 : 0);
    }
    else if (!strcmp(fh_cmd->cmd.type, "reset")) {
        snprintf(xml_buf + strlen(xml_buf), xml_size, "<power DelayInSeconds=\"%u\" value=\"reset\" />", 10);
    }
    else {
        dbg_time("%s unkonw fh_cmd->cmd.type=%s\n", __func__, fh_cmd->cmd.type);
        error_return();
    }

    pend = xml_buf + strlen(xml_buf);
    dbg_time("%.*s\n", (int)(pend - pstart),  pstart);
    //snprintf(xml_buf + strlen(xml_buf), xml_size, "\n</data>");

    if (!strcmp(fh_cmd->cmd.type, "setbootablestoragedrive") || !strcmp(fh_cmd->cmd.type, "reset")
       || !strcmp(fh_cmd->cmd.type, "configure"))
    {
        snprintf(xml_buf + strlen(xml_buf), xml_size, "\n</data>\n");
    }
    else
        snprintf(xml_buf + strlen(xml_buf), xml_size, "\n</data>");

    tx_len = qusb_noblock_write(fh_data->usb_handle, xml_buf, strlen(xml_buf), strlen(xml_buf), 3000, fh_data->ZlpAwareHost);

    if ((size_t)tx_len == strlen(xml_buf))
        return 0;

    error_return();
}

static int fh_send_cfg_cmd(struct fh_data *fh_data, const char *device_type) {
    struct fh_cmd fh_cfg_cmd;
    struct fh_cmd fh_rx_cmd;

    memset(&fh_cfg_cmd, 0x00, sizeof(fh_cfg_cmd));
    fh_cfg_cmd.cfg.type = "configure";
    fh_cfg_cmd.cfg.MemoryName = device_type;
    fh_cfg_cmd.cfg.Verbose = 0;
    fh_cfg_cmd.cfg.AlwaysValidate = 0;
    fh_cfg_cmd.cfg.SkipStorageInit = 0;
    fh_cfg_cmd.cfg.ZlpAwareHost = fh_data->ZlpAwareHost; // only sdx20 support zlp set to 0 by 20180822
    if (!strcmp(device_type, "emmc") || !strcmp(device_type, "ufs"))
    {
        fh_cfg_cmd.cfg.MaxDigestTableSizeInBytes = 8192;
        fh_cfg_cmd.cfg.MaxPayloadSizeToTargetInBytes = 1048576;
        fh_cfg_cmd.cfg.MaxPayloadSizeFromTargetInBytes = 8192;
        fh_cfg_cmd.cfg.MaxPayloadSizeToTargetInByteSupported = 1048576;
    }
    else
    {
        fh_cfg_cmd.cfg.MaxDigestTableSizeInBytes = 2048;
        fh_cfg_cmd.cfg.MaxPayloadSizeToTargetInBytes = 8192;
        fh_cfg_cmd.cfg.MaxPayloadSizeFromTargetInBytes = 2048;
        fh_cfg_cmd.cfg.MaxPayloadSizeToTargetInByteSupported = 8192;
    }

    fh_send_cmd(fh_data, &fh_cfg_cmd);
    if (fh_wait_response_cmd(fh_data, &fh_rx_cmd, 3000) != 0)
        error_return();

    if (!strcmp(fh_rx_cmd.response.value, "NAK") && fh_rx_cmd.response.MaxPayloadSizeToTargetInBytes) {
         fh_cfg_cmd.cfg.MaxPayloadSizeToTargetInBytes = fh_rx_cmd.response.MaxPayloadSizeToTargetInBytes;
         fh_cfg_cmd.cfg.MaxPayloadSizeToTargetInByteSupported = fh_rx_cmd.response.MaxPayloadSizeToTargetInBytes;

        fh_send_cmd(fh_data, &fh_cfg_cmd);
        if (fh_wait_response_cmd(fh_data, &fh_rx_cmd, 3000) != 0)
            error_return();
    }

    if (strcmp(fh_rx_cmd.response.value, "ACK") != 0)
        error_return();

    fh_data->MaxPayloadSizeToTargetInBytes = fh_cfg_cmd.cfg.MaxPayloadSizeToTargetInBytes;

    return 0;
}

static int fh_send_setbootablestoragedrive_cmd(struct fh_data *fh_data) {
    struct fh_cmd fh_0_cmd;
    fh_0_cmd.cmd.type = "setbootablestoragedrive";

    return fh_send_cmd(fh_data, &fh_0_cmd);
}

static int fh_send_reset_cmd(struct fh_data *fh_data) {
    struct fh_cmd fh_reset_cmd;
    fh_reset_cmd.cmd.type = "reset";

    return fh_send_cmd(fh_data, &fh_reset_cmd);
}

static int fh_send_rawmode_image(struct fh_data *fh_data, const struct fh_cmd *fh_cmd, unsigned timeout) {
    char full_path[512] = {0};
    char *ptmp;
    FILE *fp;
    size_t filesize, filesend;
    int retval = -1;

    void *pbuf = malloc(fh_data->MaxPayloadSizeToTargetInBytes);
    if (pbuf == NULL)
    {
        error_return();
    }

    snprintf(full_path, sizeof(full_path), "%.255s/%.240s", fh_data->firehose_dir, fh_cmd->program.filename);
    while((ptmp = strchr(full_path, '\\'))) {
        *ptmp = '/';
    }

    fp = unzip_fopen(full_path, "rb");
    if (!fp) {
        dbg_time("fail to fopen %s, errno: %d (%s)\n", full_path, errno, strerror(errno));
        error_return();
    }

    if (fh_cmd->program.sparse != NULL && !strncasecmp(fh_cmd->program.sparse, "true", 4))
    {
        filesize = fh_cmd->program.UNSPARSE_FILE_SIZE;
        filesend = 0;
        fseek(fp, fh_cmd->program.file_sector_offset, SEEK_SET);
    }
    else
    {
        fseek(fp, 0, SEEK_END);
        filesize = ftell(fp);
        filesend = 0;
        fseek(fp, 0, SEEK_SET);
    }

    dbg_time("send %s, filesize=%zd\n", full_path + strlen(fh_data->firehose_dir) + 1, filesize);

    if (strEndsWith(full_path, "gpt_empty0.bin"))
    {
        filesize -= 512;
    }

    int idx = -1;

    if (fh_cmd->program.sparse != NULL && !strncasecmp(fh_cmd->program.sparse, "true", 4))
    {
        while (filesend < filesize)
        {
            size_t reads,  writes;
            uint32_t  chunk_data_sz, data_sz = 0;
            chunk_header_t chunk_header;

            if (fread(&chunk_header, 1, sizeof(chunk_header_t), fp) != sizeof(chunk_header_t)) {
                error_set_and_out(-8);
            }
            chunk_header.chunk_type = le_uint16(chunk_header.chunk_type);
            chunk_header.chunk_sz = le_uint16(chunk_header.chunk_sz);
            chunk_header.total_sz = le_uint32(chunk_header.total_sz);
            chunk_data_sz = chunk_header.chunk_sz *  SparseImgData->blk_sz;

            if (chunk_header.chunk_type == SPARSE_CHUNK_TYPE_RAW) {
                 while (data_sz < chunk_data_sz) {
                    reads = fh_data->MaxPayloadSizeToTargetInBytes;
                    if (data_sz + reads > chunk_data_sz)
                        reads = chunk_data_sz - data_sz;
                    if (fread(pbuf, 1, reads , fp) != reads) {
                        error_set_and_out(-9);
                        break;
                    }

                    writes = qusb_noblock_write(fh_data->usb_handle, pbuf, reads, reads, timeout, fh_data->ZlpAwareHost);
                    if (reads != writes) {
                        error_set_and_out(-10);
                        break;
                    }
                    data_sz += writes;
                    filesend += writes;
                    update_transfer_bytes(writes);
                    if (!((++idx) % 0x80)) {
                        printf(".");
                        fflush(stdout);
                    }                   
                 }
            }
            else if (chunk_header.chunk_type == SPARSE_CHUNK_TYPE_FILL || chunk_header.chunk_type == SPARSE_CHUNK_TYPE_ZERO) {
                uint32_t i, fillValue = 0;

                if (chunk_header.chunk_type == SPARSE_CHUNK_TYPE_FILL) {
                    if (fread(pbuf, 1, sizeof(fillValue), fp) != sizeof(fillValue)) {
                        error_set_and_out(-11);
                        break;
                    }
                }
                else if (chunk_header.chunk_type == SPARSE_CHUNK_TYPE_ZERO) {
                }

                for (i = 0; i < (fh_data->MaxPayloadSizeToTargetInBytes/sizeof(fillValue)); i++)
                    ((uint32_t *)pbuf)[i] = fillValue;

                 while (data_sz < chunk_data_sz) {
                    reads = fh_data->MaxPayloadSizeToTargetInBytes;
                    if (data_sz + reads > chunk_data_sz)
                        reads = chunk_data_sz - data_sz;

                    writes = qusb_noblock_write(fh_data->usb_handle, pbuf, reads, reads, timeout, fh_data->ZlpAwareHost);
                    if (reads != writes) {
                        error_set_and_out(-12);
                        break;
                    }
                    data_sz += writes;
                    filesend += writes;
                    update_transfer_bytes(writes);
                    if (!((++idx) % 0x80)) {
                        printf(".");
                        fflush(stdout);
                    }                   
                 }
            }
            else {
                dbg_time("%s unknow chunk_type = %x\n", __func__, chunk_header.chunk_type);
                error_set_and_out(-8);
            }
        }
    }
    else
    {
        while (filesend < filesize) {
            size_t reads,  writes;

            reads = fh_data->MaxPayloadSizeToTargetInBytes;
            if (reads > (filesize - filesend))
                reads = filesize - filesend;
            reads = fread(pbuf, 1, reads, fp);
            if (reads <= 0) {
                error_set_and_out(-13);
                break;
            }

            if (reads % fh_cmd->program.SECTOR_SIZE_IN_BYTES) {
                memset((uint8_t *)pbuf + reads, 0, fh_cmd->program.SECTOR_SIZE_IN_BYTES - (reads % fh_cmd->program.SECTOR_SIZE_IN_BYTES));
                reads +=  fh_cmd->program.SECTOR_SIZE_IN_BYTES - (reads % fh_cmd->program.SECTOR_SIZE_IN_BYTES);
            }

            writes = qusb_noblock_write(fh_data->usb_handle, pbuf, reads, reads, timeout, fh_data->ZlpAwareHost);
            if (reads != writes) {
                dbg_time("%s send fail reads=%zd, writes=%zd\n", __func__, reads, writes);
                dbg_time("%s send fail filesend=%zd, filesize=%zd\n", __func__, filesend, filesize);
                error_set_and_out(-14);
            }
            filesend += reads;
            update_transfer_bytes(writes);
            if (!((++idx) % 0x80)) {
                printf(".");
                fflush(stdout);
            }
        }
    }

    printf("\n");
    show_progress();
    dbg_time("send finished\n");

out:
    if (fp) unzip_fclose(fp);
    safe_free(pbuf);

    if (filesend >= filesize)
        return 0;
    return retval;
}

static int fh_process_erase(struct fh_data *fh_data, const struct fh_cmd *fh_cmd)
{
    struct fh_cmd fh_rx_cmd;
    unsigned timeout = 15000; //8+8 MCP need more time

    fh_send_cmd(fh_data, fh_cmd);
    if (fh_wait_response_cmd(fh_data, &fh_rx_cmd, timeout) != 0) //SDX55 need 4 seconds
        error_return();
    if (strcmp(fh_rx_cmd.response.value, "ACK"))
        error_return();

    return 0;
}

static int fh_process_patch(struct fh_data *fh_data, const struct fh_cmd *fh_cmd)
{
    struct fh_cmd fh_rx_cmd;
    unsigned timeout = 15000; //8+8 MCP need more time

    fh_send_cmd(fh_data, fh_cmd);
    if (fh_wait_response_cmd(fh_data, &fh_rx_cmd, timeout) != 0) //SDX55 need 4 seconds
    {
        dbg_time("fh_process_patch : fh_wait_response_cmd fail\n");
        error_return();
    }
    if (strcmp(fh_rx_cmd.response.value, "ACK"))
    {
        dbg_time("fh_process_patch : response should be ACK\n");
        error_return();
    }

    return 0;
}

static int fh_process_sparse_program(struct fh_data *fh_data, const struct fh_cmd *fh_cmd)
{
    char full_path[512];
    char *ptmp;
    FILE *fp;
    sparse_header_t sparse_header;
    uint32_t start_chunk_offset = 0;
    uint32_t m;
    int retval = 0;

    snprintf(full_path, sizeof(full_path), "%.255s/%.240s", fh_data->firehose_dir, fh_cmd->program.filename);
    while((ptmp = strchr(full_path, '\\'))) {
        *ptmp = '/';
    }

    fp = unzip_fopen(full_path, "rb");
    if (!fp) {
        dbg_time("%s fail to fopen %s, errno: %d (%s)\n", __func__, full_path, errno, strerror(errno));
        error_set_and_out(-1);
    }

    if (fread(&sparse_header, 1, sizeof(sparse_header_t), fp) != sizeof(sparse_header_t)) {
        dbg_time("%s fread sparse_header_t failed\n", __func__);
        error_set_and_out(-2);
    }

    if (le_uint32(sparse_header.magic) != SPARSE_HEADER_MAGIC
        || le_uint32(sparse_header.major_version) != 1
        || le_uint32(sparse_header.minor_version) != 0
        || le_uint32(sparse_header.file_hdr_sz) != sizeof(sparse_header_t)
        || le_uint32(sparse_header.chunk_hdr_sz ) != sizeof(chunk_header_t)
        || le_uint32(sparse_header.image_checksum ) != 0
        ) {
        dbg_time("sparse_header->magic = %0x\n", le_uint32(sparse_header.magic));
        dbg_time("sparse_header->major_version = %0x\n", le_uint32(sparse_header.major_version));
        dbg_time("sparse_header->minor_version = %0x\n", le_uint32(sparse_header.minor_version));
        dbg_time("sparse_header->file_hdr_sz = %d\n", le_uint32(sparse_header.file_hdr_sz));
        dbg_time("sparse_header->chunk_hdr_sz = %d\n", le_uint32(sparse_header.chunk_hdr_sz));
        dbg_time("sparse_header->blk_sz = %d\n", le_uint32(sparse_header.blk_sz));
        dbg_time("sparse_header->total_blks = %d\n", le_uint32(sparse_header.total_blks));
        dbg_time("sparse_header->total_chunks = %d\n", le_uint32(sparse_header.total_chunks));
        dbg_time("sparse_header->image_checksum = %d\n", le_uint32(sparse_header.image_checksum));
        error_set_and_out(-3);
    }
    sparse_header.total_chunks = le_uint32(sparse_header.total_chunks);
    sparse_header.blk_sz = le_uint32(sparse_header.blk_sz);

    m = sizeof(SparseImgParam) + sparse_header.total_chunks * sizeof(chunk_polymerization_param);
    safe_free(SparseImgData);
    SparseImgData = malloc(m);
    if (SparseImgData == NULL) {
        error_set_and_out(-4);
    }
    memset(SparseImgData, 0, m);
    SparseImgData->blk_sz = sparse_header.blk_sz;

    for (m = 0; m < sparse_header.total_chunks; m++)
    {
        chunk_header_t chunk_header;
        chunk_polymerization_param *tmp = &SparseImgData->chunk_polymerization_data[SparseImgData->total_count];
        int merge_done = 0;

        if (fread(&chunk_header, 1, sizeof(chunk_header_t), fp) !=sizeof(chunk_header_t)) {
            error_set_and_out(-4);
        }

        chunk_header.chunk_type = le_uint16(chunk_header.chunk_type);
        chunk_header.chunk_sz = le_uint16(chunk_header.chunk_sz);
        chunk_header.total_sz = le_uint32(chunk_header.total_sz);
        #if 0
        printf("chunk_header: chunk_type = %0x ", chunk_header.chunk_type);
        printf("chunk_sz = %u ", chunk_header.chunk_sz);
        printf("total_sz = %u\n", chunk_header.total_sz);
        #endif

        if (chunk_header.chunk_type == SPARSE_CHUNK_TYPE_RAW
            || chunk_header.chunk_type == SPARSE_CHUNK_TYPE_FILL) {

            if (chunk_header.chunk_type == SPARSE_CHUNK_TYPE_RAW) {
                if (chunk_header.total_sz != chunk_header.chunk_sz * sparse_header.blk_sz + sizeof(chunk_header_t)) {
                    error_set_and_out(-5);
                }
            }
            else if (chunk_header.chunk_type == SPARSE_CHUNK_TYPE_FILL) {
                if (chunk_header.total_sz != 4 + sizeof(chunk_header_t)) {
                    error_set_and_out(-6);
                }
            }

            tmp->total_chunk_sz += chunk_header.chunk_sz;
            tmp->total_chunk_count++;
        }
        else if (chunk_header.chunk_type == SPARSE_CHUNK_TYPE_ZERO)
        {
            if (chunk_header.total_sz != sizeof(chunk_header_t)) {
                error_set_and_out(-7);
            }

#if SPARSE_CHUNK_MERGE_POLICY == SPARSE_CHUNK_MERGE_RAW_FILL_ZERO
            tmp->total_chunk_sz += chunk_header.chunk_sz;
            tmp->total_chunk_count++;
#elif SPARSE_CHUNK_MERGE_POLICY == SPARSE_CHUNK_MERGE_NONE
            tmp->total_chunk_sz += chunk_header.chunk_sz;
            tmp->total_chunk_count++;
#elif SPARSE_CHUNK_MERGE_POLICY == SPARSE_CHUNK_MERGE_RAW_FILL
            if (tmp->total_chunk_count > 0)
                merge_done = 1;
#endif
        }
        else {
            dbg_time("%s unknow chunk_type = %x\n", __func__, chunk_header.chunk_type);
            error_set_and_out(-8);
        }

 #if SPARSE_CHUNK_MERGE_POLICY == SPARSE_CHUNK_MERGE_NONE
        merge_done = 1;
#endif
       if (m == sparse_header.total_chunks - 1)
            merge_done = 1;

        if (tmp->total_chunk_count == 1 && tmp->file_sector_offset == 0) {
            tmp->file_sector_offset = ftell(fp) - sizeof(chunk_header_t);
            tmp->start_chunk_offset = start_chunk_offset;
        }

        if (merge_done && (tmp->total_chunk_count > 0)) {
            tmp->total_sz = tmp->total_chunk_sz * sparse_header.blk_sz;
            //dbg_time("[%d] total_chunk_sz=%u, total_chunk_count=%u, file_sector_offset=%ld, total_sz=%ld\n",
            //     SparseImgData->total_count, tmp->total_chunk_sz, tmp->total_chunk_count, tmp->file_sector_offset, tmp->total_sz);
           SparseImgData->total_count++;
        }

        start_chunk_offset += chunk_header.chunk_sz;
        fseek(fp, chunk_header.total_sz - sizeof(chunk_header_t), SEEK_CUR);
    }

out:
    if (fp) unzip_fclose(fp);
    if (retval && SparseImgData) {
        safe_free(SparseImgData);
    }
    if (retval) {
        dbg_time("%s retval=%d\n", __func__, retval);
    }
    return retval;
}

static int fh_process_program(struct fh_data *fh_data, struct fh_cmd *fh_cmd)
{
    struct fh_cmd fh_rx_cmd;

    if (fh_cmd->program.sparse != NULL && !strncasecmp(fh_cmd->program.sparse, "true", 4))
    {
        uint32_t i;
        uint32_t start_sector = fh_cmd->program.start_sector;

        if (fh_process_sparse_program(fh_data, fh_cmd)) {
            error_return();
        }
       
        for (i=0;i<SparseImgData->total_count;i++)
        {
            chunk_polymerization_param *tmp = &SparseImgData->chunk_polymerization_data[i];

            fh_cmd->program.file_sector_offset = tmp->file_sector_offset;
            fh_cmd->program.start_sector = start_sector + (tmp->start_chunk_offset * (SparseImgData->blk_sz / fh_cmd->program.SECTOR_SIZE_IN_BYTES));
            fh_cmd->program.UNSPARSE_FILE_SIZE = tmp->total_sz;
            fh_cmd->program.num_partition_sectors = fh_cmd->program.UNSPARSE_FILE_SIZE / fh_cmd->program.SECTOR_SIZE_IN_BYTES;
            if (fh_cmd->program.UNSPARSE_FILE_SIZE % fh_cmd->program.SECTOR_SIZE_IN_BYTES)
                fh_cmd->program.num_partition_sectors += 1;

            fh_send_cmd(fh_data, fh_cmd);
            if (fh_wait_response_cmd(fh_data, &fh_rx_cmd, 3000) != 0) {
                dbg_time("fh_wait_response_cmd fail\n");
                error_return();
            }
            if (strcmp(fh_rx_cmd.response.value, "ACK")) {
                dbg_time("response should be ACK\n");
                error_return();
            }
            if (fh_rx_cmd.response.rawmode != 1) {
                dbg_time("response should be rawmode true\n");
                error_return();
            }
            if (fh_send_rawmode_image(fh_data, fh_cmd, 15000)) {
                dbg_time("fh_send_rawmode_image fail\n");
                error_return();
            }
            if (fh_wait_response_cmd(fh_data, &fh_rx_cmd, 6000) != 0) {
                dbg_time("fh_wait_response_cmd fail\n");
                error_return();
            }
            if (strcmp(fh_rx_cmd.response.value, "ACK")) {
                dbg_time("response should be ACK\n");
                error_return();
            }
            if (fh_rx_cmd.response.rawmode != 0) {
                dbg_time("response should be rawmode false\n");
                error_return();
            }
        }
    }
    else
    {
        fh_send_cmd(fh_data, fh_cmd);
        if (fh_wait_response_cmd(fh_data, &fh_rx_cmd, 3000) != 0) {
            dbg_time("fh_wait_response_cmd fail\n");
            error_return();
        }
        if (strcmp(fh_rx_cmd.response.value, "ACK")) {
            dbg_time("response should be ACK\n");
            error_return();
        }
        if (fh_rx_cmd.response.rawmode != 1) {
            dbg_time("response should be rawmode true\n");
            error_return();
        }
        if (fh_send_rawmode_image(fh_data, fh_cmd, 15000)) {
            dbg_time("fh_send_rawmode_image fail\n");
            error_return();
        }
        if (fh_wait_response_cmd(fh_data, &fh_rx_cmd, 6000) != 0) {
            dbg_time("fh_wait_response_cmd fail\n");
            error_return();
        }
        if (strcmp(fh_rx_cmd.response.value, "ACK")) {
            dbg_time("response should be ACK\n");
            error_return();
        }
        if (fh_rx_cmd.response.rawmode != 0) {
            dbg_time("response should be rawmode false\n");
            error_return();
        }
    }

    free(fh_cmd->program.filename);

    return 0;
}

int firehose_main(const char *firehose_dir, void *usb_handle, unsigned qusb_zlp_mode) {
    unsigned x;
    char rawprogram_full_path[512];
    char *xmlfile_list[32];
    char xmlfile_tmp[32];
    unsigned xmlfile_cnt = 0;
    struct fh_cmd fh_rx_cmd;
    struct fh_data *fh_data;
    long long filesizes = 0;
    long filesize = 0;
    unsigned max_num_partition_sectors = 0;
    static pthread_t recv_cmd_tid;
    int first_earse_and_last_programm_SBL = 0;
    int rawprogram_unsparse_exist = 0;

    fh_data = (struct fh_data *)malloc(sizeof(struct fh_data));
    if (!fh_data)
        error_return();

    memset(fh_data, 0x00, sizeof(struct fh_data));
    fh_data->firehose_dir = firehose_dir;
    fh_data->usb_handle = usb_handle;
    fh_data->xml_tx_size = sizeof(fh_data->xml_tx_buf);
    fh_data->xml_rx_size = sizeof(fh_data->xml_rx_buf);
    fh_data->ZlpAwareHost = qusb_zlp_mode;

    {
        if (q_module_packet_sign)
        {
            for (x = 0; x < 10; x++) {
                snprintf(xmlfile_tmp, sizeof(xmlfile_tmp), "rawprogram%u_secboot", x);  //use rawprogram%u Adaptation rawprogram%u_xxx  for AG215S-GLR
                if (!qfile_find_file(firehose_dir, xmlfile_tmp, ".xml", &xmlfile_list[xmlfile_cnt])) {
                    continue;
                }
                xmlfile_cnt++;
            }

            if (!qfile_find_file(firehose_dir, "secboot_", ".xml", &xmlfile_list[xmlfile_cnt]))
            {
                dbg_time("secboot rawprogram nand file failed.\n");
                // error_return();
            }
            else
            {
                xmlfile_cnt++;
            }
        }
        else
        {
            for (x = 0; x < 10; x++) {
                snprintf(xmlfile_tmp, sizeof(xmlfile_tmp), "rawprogram_unsparse%u", x);  //smart  SA885GAPNA
                if (!qfile_find_file(firehose_dir, xmlfile_tmp, ".xml", &xmlfile_list[xmlfile_cnt])) {
                    continue;
                }
                xmlfile_cnt++;
                rawprogram_unsparse_exist++;
            }

            if (rawprogram_unsparse_exist == 0)
            {
                if (!qfile_find_file(firehose_dir, "rawprogram_", ".xml", &xmlfile_list[xmlfile_cnt])) {
                    dbg_time("retrieve rawprogram namd file failed.\n");
                    //error_return();
                }
                else
                {
                    xmlfile_cnt++;
                }
            }

            if (!qfile_find_file(firehose_dir, "firehose-rawprogram", ".xml", &xmlfile_list[xmlfile_cnt])) {
                dbg_time("retrieve rawprogram namd file failed.\n");
                //error_return();
            }
            else
                xmlfile_cnt++;

            for (x = 0; x < 10; x++) {
                snprintf(xmlfile_tmp, sizeof(xmlfile_tmp), "rawprogram%u", x);  //use rawprogram%u Adaptation rawprogram%u_xxx
                if (!qfile_find_file(firehose_dir, xmlfile_tmp, ".xml", &xmlfile_list[xmlfile_cnt])) {
                    continue;
                }
                xmlfile_cnt++;
            }
        }

        if (!qfile_find_file(firehose_dir, "patch_", ".xml", &xmlfile_list[xmlfile_cnt])
            && !qfile_find_file(firehose_dir, "patch-", ".xml", &xmlfile_list[xmlfile_cnt])
           ) {
            dbg_time("retrieve patch namd file failed.\n");
            //error_return();
        }
        else
            xmlfile_cnt++;

        for (x = 0; x < 10; x++) {
            snprintf(xmlfile_tmp, sizeof(xmlfile_tmp), "patch%u.xml", x);
            if (!qfile_find_file(firehose_dir, xmlfile_tmp, ".xml", &xmlfile_list[xmlfile_cnt])) {
                continue;
            }
            xmlfile_cnt++;
        }

        for (x = 0; x < xmlfile_cnt; x++) {
            snprintf(rawprogram_full_path, sizeof(rawprogram_full_path), "%.255s/%.255s", firehose_dir, xmlfile_list[x]);
            free(xmlfile_list[xmlfile_cnt]);
            fh_parse_xml_file(fh_data, rawprogram_full_path);
        }

        if (fh_data->fh_cmd_count == 0)
        {
            if (fh_data)
            {
                free(fh_data);
                fh_data = NULL;
            }
            error_return();
        }
    }

    for (x = 0; x < fh_data->fh_cmd_count; x++) {
        struct fh_cmd *fh_cmd = &fh_data->fh_cmd_table[x];

        if (strstr(fh_cmd->cmd.type, "program")) {
            fh_fixup_program_cmd(fh_data, fh_cmd, &filesize);
            if (fh_cmd->program.num_partition_sectors == 0)
            {
                if (fh_data)
                {
                    free(fh_data);
                    fh_data = NULL;
                }
                error_return();
            }

            //calc files size
            filesizes += filesize;
        }
        else if (strstr(fh_cmd->cmd.type, "erase")) {
            if ((fh_cmd->erase.num_partition_sectors + fh_cmd->erase.start_sector) > max_num_partition_sectors)
                max_num_partition_sectors = (fh_cmd->erase.num_partition_sectors + fh_cmd->erase.start_sector);
        }
    }

    if (socketpair( AF_LOCAL, SOCK_STREAM, 0, fh_recv_cmd_sk))
    {
        if (fh_data)
        {
            free(fh_data);
            fh_data = NULL;
        }
        error_return();
    }
    fcntl(fh_recv_cmd_sk[0], F_SETFL, O_NONBLOCK);
    if (pthread_create(&recv_cmd_tid, NULL, fh_recv_cmd_thread, (void *)fh_data))
        error_return();
    set_transfer_allbytes(filesizes);
    //must first read <log from mdm9x07, then send <configure, and 1 second is not enough
    fh_recv_cmd(fh_data, &fh_rx_cmd, 3000, 1);
    while (fh_recv_cmd(fh_data, &fh_rx_cmd, 1000, 1) == 0);

    if (fh_send_cfg_cmd(fh_data, q_device_type))
        error_return();

    if (!strcmp(q_device_type, "nand"))
        first_earse_and_last_programm_SBL = 1;

    if (first_earse_and_last_programm_SBL || q_erase_all_before_download) {
        for (x = 0; x < fh_data->fh_cmd_count; x++) {
            struct fh_cmd *fh_cmd = &fh_data->fh_cmd_table[x];

            if (!strstr(fh_cmd->cmd.type, "erase"))
                continue;

             if (fh_cmd->erase.start_sector != 0)     //Pre erase start_sector == 0 partition
                continue;

            if (q_erase_all_before_download) {
                fh_xml_set_value(fh_cmd->xml_original_data, "num_partition_sectors", max_num_partition_sectors);
                if (fh_cmd->erase.last_sector) {
                    fh_xml_set_value(fh_cmd->xml_original_data, "last_sector", max_num_partition_sectors - 1);
                }
            }

             if (fh_process_erase(fh_data, fh_cmd))
                error_return();
        }
    }

    for (x = 0; x < fh_data->fh_cmd_count; x++) {
        const struct fh_cmd *fh_cmd = &fh_data->fh_cmd_table[x];

        if (strstr(fh_cmd->cmd.type, "vendor")) {
            fh_send_cmd(fh_data, fh_cmd);
            if (fh_wait_response_cmd(fh_data, &fh_rx_cmd, 6000) != 0)
                error_return();
            if (strcmp(fh_rx_cmd.response.value, "ACK"))
                error_return();
        }
    }

    if (!q_erase_all_before_download) {
        for (x = 0; x < fh_data->fh_cmd_count; x++) {
            struct fh_cmd *fh_cmd = &fh_data->fh_cmd_table[x];

            if (!strstr(fh_cmd->cmd.type, "erase"))
                continue;

            if (fh_cmd->erase.SECTOR_SIZE_IN_BYTES == 0)  //防止BG95 误烧录 jira id: STMDM9205-5237 不能fh_cmd->erase.num_partition_sectors == 0， 因为<erase SECTOR_SIZE_IN_BYTES="512" label="erase whole disk" physical_partition_number="0" start_sector="0" /> 需要写到模块
                continue;

            if (first_earse_and_last_programm_SBL)
            {
                if (fh_cmd->erase.start_sector == 0)    //Skip erase start_sector == 0 partition
                    continue;
            }

            if (fh_process_erase(fh_data, fh_cmd))
                error_return();
        }
    }

    for (x = 0; x < fh_data->fh_cmd_count; x++) {
        struct fh_cmd *fh_cmd = &fh_data->fh_cmd_table[x];

        if (!strstr(fh_cmd->cmd.type, "program"))
            continue;

        if (first_earse_and_last_programm_SBL && fh_cmd->program.start_sector == 0)
            continue;

        if (fh_process_program(fh_data, fh_cmd)) {
            safe_free(SparseImgData);
            error_return();
        }
    }

    if (first_earse_and_last_programm_SBL) {
        for (x = 0; x < fh_data->fh_cmd_count; x++) {
            struct fh_cmd *fh_cmd = &fh_data->fh_cmd_table[x];

            if (!strstr(fh_cmd->cmd.type, "program"))
                continue;

            if (fh_cmd->program.start_sector != 0)
                continue;

            if (fh_process_program(fh_data, fh_cmd)) {
                safe_free(SparseImgData);
                error_return();
            }
        }
    }

    if (fh_data->fh_patch_count) {
        for (x = 0; x < fh_data->fh_cmd_count; x++) {
            const struct fh_cmd *fh_cmd = &fh_data->fh_cmd_table[x];

            if (!strstr(fh_cmd->cmd.type, "patch"))
                continue;

            if (fh_process_patch(fh_data, fh_cmd))
                error_return();
        }
    }

    if (strcmp(q_device_type, "nand")) {
        fh_send_setbootablestoragedrive_cmd(fh_data);
        if (fh_wait_response_cmd(fh_data, &fh_rx_cmd, 3000) != 0)
            error_return();
    }

    fh_send_reset_cmd(fh_data);
    if (fh_wait_response_cmd(fh_data, &fh_rx_cmd, 3000) != 0)
        error_return();
    while (fh_recv_cmd(fh_data, &fh_rx_cmd, 1000, 1) == 0); //required by sdx20

    free(fh_data);

    //pthread_join(recv_cmd_tid, NULL);
    close(fh_recv_cmd_sk[0]);
    close(fh_recv_cmd_sk[1]);
    return 0;
}
