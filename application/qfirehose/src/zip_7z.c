#include <inttypes.h>
#include <stdio.h>  
#include <stdlib.h>  
#include <stdint.h>  
#include <string.h>  
#include <fcntl.h>
#include <unistd.h>
#include <endian.h> 
#include <errno.h> 
#include <sys/stat.h> 
#include "usb_linux.h"

#define save_max_fp 8
struct unzip_ctx {
    char line[1024];
    char save_zip_file[512];
    char save_unzip_dir[512];
    int is_7z_file;
    FILE *save_fp[save_max_fp];
    char save_filename[save_max_fp][512];
};

struct unzip_ctx *p_unzip_ctx;

static int create_and_write_filesize(const char *file, long sz) {
    char line[32];

    int fd = open(file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd == -1) {
        dbg_time("open(%s) fail! errno: %d (%s)\n", line, errno, strerror(errno));
        return -1;
    }
     snprintf(line, sizeof(line), "%ld\n", sz);
     if (write(fd, line, strlen(line))) {};
     close(fd);

     return 0;
}

void unzip_rm_tmp_dir(void) {
    struct unzip_ctx *ctx = p_unzip_ctx;

    if (!ctx)
        return;

    snprintf(ctx->line, sizeof(ctx->line), "rm -rf %.64s", ctx->save_unzip_dir);
    if (system(ctx->line)) {};
    free(ctx);
    p_unzip_ctx = NULL;
}

int unzip_to_tmp_dir(const char *zipfile, const char *tmp_dir)
{
    int n;
    FILE *fp;
    int start_parse = 0;
    struct unzip_ctx * ctx;

    if (!strEndsWith(zipfile, ".7z") && !strEndsWith(zipfile, ".zip"))
        return -1;

    ctx = malloc(sizeof(struct unzip_ctx));
    if (!ctx)
        return -1;
    memset(ctx, 0, sizeof(struct unzip_ctx));

    snprintf(ctx->line, sizeof(ctx->line), "rm -rf %s", tmp_dir);    
    if (system(ctx->line)) {};
    if (mkdir(tmp_dir, 0755) != 0) {
        dbg_time("mkdir(%s) fail! errno: %d (%s)\n", tmp_dir, errno, strerror(errno));
        free(ctx);
        return -1;
    }

    if (strEndsWith(zipfile, ".7z")) {
        ctx->is_7z_file = 1;
        snprintf(ctx->line, sizeof(ctx->line), "7z l  %s", zipfile);
    }
    else
        snprintf(ctx->line, sizeof(ctx->line), "unzip -l %s", zipfile);
    fp = popen(ctx->line, "r");
    if (fp == NULL) {
        dbg_time("popen(%s) fail! errno: %d (%s)\n", ctx->line, errno, strerror(errno));
        free(ctx);
        return -1;
    }

    start_parse = 0;
    while (fgets(ctx->line, sizeof(ctx->line), fp)) {
        long Length;
        uint32_t Compressed;
        char Date[16];
        char Time[16];
        char Attr[16];
        char Name[128];

/*
  Length      Date    Time    Name
---------  ---------- -----   ----
    36553  2024-03-06 15:35   contents.xml

   Date      Time    Attr         Size   Compressed  Name
------------------- ----- ------------ ------------  ------------------------
2024-03-11 11:50:16 D....            0            0  RG650VEU01AA_VD_RDK-B_0306_factory
*/
        if (start_parse == 0) {
            if (!strncmp(ctx->line, "---------", 8)) { //start
                start_parse = 1;
           }
           continue;
        }

        if (!strncmp(ctx->line, "---------", 8)) { //end
            start_parse = 0;
            continue;
        }

        if (ctx->is_7z_file) {
            Name[0] = '\0';
            n = sscanf(ctx->line, "%s %s %s %ld %u %s",  Date, Time, Attr, &Length, &Compressed, Name);
            if (n == 4)
                n = sscanf(ctx->line, "%s %s %s %ld %s",  Date, Time, Attr, &Length, Name);
            if (Name[0] == '\0') {
                dbg_time("error: n=%d '%s'\n", n, ctx->line);
                continue;
            }
        }
        else {
            Name[0] = '\0';
            n = sscanf(ctx->line, "%ld %s %s %s", &Length, Date, Time, Name);
            if (Name[0] == '\0') {
                dbg_time("error: n=%d '%s'\n", n, ctx->line);
                continue;
            }
        }

        snprintf(ctx->line, sizeof(ctx->line), "%s/%s", tmp_dir, Name);
       if (Length == 0) { //dir
            if (mkdir(ctx->line, 0755) != 0) {
                dbg_time("mkdir(%s) fail! errno: %d (%s)\n", ctx->line, errno, strerror(errno));
                goto out;
            }
        }
        else if (create_and_write_filesize(ctx->line, Length)) {
                goto out;
        }
    }

    pclose(fp);
    strcpy(ctx->save_zip_file, zipfile);
    strcpy(ctx->save_unzip_dir, tmp_dir);
    p_unzip_ctx = ctx;
    return 0;

out:
    pclose(fp);
    free(ctx);
    return -1;
}

FILE * unzip_fopen(const char *file, const char *mode) {
    struct unzip_ctx *ctx = p_unzip_ctx;
    FILE *fp;

    if (ctx && ctx->save_zip_file[0] && strStartsWith(file, ctx->save_unzip_dir)) {
        //update/firehose/../NON-HLOS.bin
        const char *firehose = "/firehose/..";
        char *upper = strstr(file, firehose);

        if (upper) {
            memcpy(upper, upper+strlen(firehose), strlen(upper+strlen(firehose))+1);
        }
        
        if (ctx->is_7z_file) {
            snprintf(ctx->line, sizeof(ctx->line), "7z x %s %s -so > %s", ctx->save_zip_file, file + strlen(ctx->save_unzip_dir) + 1, file);
        }
        else {
            snprintf(ctx->line, sizeof(ctx->line), "unzip -p %s %s > %s", ctx->save_zip_file, file + strlen(ctx->save_unzip_dir) + 1, file);
        }

        if (system(ctx->line) != 0) {
            dbg_time("system(%s) fail! errno: %d (%s)\n", ctx->line, errno, strerror(errno));
            return NULL;
        }

        fp = fopen(file, mode);
        if (fp != NULL) {
            unsigned int i;

            for (i = 0; i < save_max_fp; i++) {
                if (ctx->save_fp[i] == NULL) {
                    ctx->save_fp[i] = fp;
                    strcpy(ctx->save_filename[i], file);
                    break;
                }
            }            
        }

        return fp;
    }

    return fopen(file, mode);
}

void unzip_fclose(FILE *fp) {
    struct unzip_ctx *ctx = p_unzip_ctx;
    unsigned int i;

    for (i = 0; i < save_max_fp; i++) {
        if (ctx && ctx->save_fp[i] == fp) {
            long filesz;

            ctx->save_fp[i] = NULL;
            fseek(fp, 0, SEEK_END);
            filesz = ftell(fp);
            fclose(fp);
            if (create_and_write_filesize(ctx->save_filename[i], filesz)) {
            }
            return;
        }
    }
    fclose(fp);
}

long unzip_get_file_size(const char *file) {
    struct unzip_ctx *ctx = p_unzip_ctx;
    long filesize = 0;
    FILE *fp = fopen(file, "rb");

    if (fp == NULL)
        return 0;

    if (ctx && ctx->save_zip_file[0] && strStartsWith(file, ctx->save_unzip_dir)) {
        char value[32];

        if (fread(value, 1, sizeof(value), fp) > 1)
            filesize = atol(value);
    }
    else {
        fseek(fp, 0, SEEK_END);
        filesize = ftell(fp);
    }

    fclose(fp);
    return filesize;
}
