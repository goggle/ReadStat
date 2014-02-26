

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "readstat_io.h"
#include "readstat_sas.h"


#define SAS_ALIGNMENT_OFFSET_4  0x33

#define SAS_FILE_FORMAT_UNIX    '1'
#define SAS_FILE_FORMAT_WINDOWS '2'

#define SAS_ENDIAN_BIG       0x00
#define SAS_ENDIAN_LITTLE    0x01

#define SAS_PAGE_TYPE_META   0x0000
#define SAS_PAGE_TYPE_DATA   0x0100
#define SAS_PAGE_TYPE_MIX    0x0200
#define SAS_PAGE_TYPE_AMD    0x0400
#define SAS_PAGE_TYPE_MASK   0x0F00

#define SAS_PAGE_TYPE_META2  0x4000
#define SAS_PAGE_TYPE_COMP   0x9000

#define SAS_COLUMN_TYPE_NUM  0x01
#define SAS_COLUMN_TYPE_CHR  0x02

#define SAS_COMPRESSION_NONE   0x00
#define SAS_COMPRESSION_TRUNC  0x01
#define SAS_COMPRESSION_RLE    0x04

#define SAS_RLE_COMMAND_COPY64          0
#define SAS_RLE_COMMAND_INSERT_BLANK17  6
#define SAS_RLE_COMMAND_INSERT_ZERO17   7
#define SAS_RLE_COMMAND_COPY1           8
#define SAS_RLE_COMMAND_COPY17          9
#define SAS_RLE_COMMAND_COPY33         10
#define SAS_RLE_COMMAND_COPY49         11
#define SAS_RLE_COMMAND_INSERT_BYTE3   12
#define SAS_RLE_COMMAND_INSERT_AT2     13
#define SAS_RLE_COMMAND_INSERT_BLANK2  14
#define SAS_RLE_COMMAND_INSERT_ZERO2   15

static char sas7bdat_magic_number[32] = {
    0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,   0xc2, 0xea, 0x81, 0x60,
    0xb3, 0x14, 0x11, 0xcf,   0xbd, 0x92, 0x08, 0x00,
    0x09, 0xc7, 0x31, 0x8c,   0x18, 0x1f, 0x10, 0x11
};

static char sas7bcat_magic_number[32] = {
    0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,   0xc2, 0xea, 0x81, 0x63,
    0xb3, 0x14, 0x11, 0xcf,   0xbd, 0x92, 0x08, 0x00,
    0x09, 0xc7, 0x31, 0x8c,   0x18, 0x1f, 0x10, 0x11
};

#define SAS_SUBHEADER_SIGNATURE_ROW_SIZE       0xF7F7F7F7
#define SAS_SUBHEADER_SIGNATURE_COLUMN_SIZE    0xF6F6F6F6
#define SAS_SUBHEADER_SIGNATURE_COUNTS         0xFFFFFC00
#define SAS_SUBHEADER_SIGNATURE_COLUMN_FORMAT  0xFFFFFBFE
#define SAS_SUBHEADER_SIGNATURE_COLUMN_ATTRS   0xFFFFFFFC
#define SAS_SUBHEADER_SIGNATURE_COLUMN_TEXT    0xFFFFFFFD
#define SAS_SUBHEADER_SIGNATURE_COLUMN_LIST    0xFFFFFFFE
#define SAS_SUBHEADER_SIGNATURE_COLUMN_NAME    0xFFFFFFFF

typedef struct text_ref_s {
    int    index;
    int    offset;
    int    length;
} text_ref_t;

typedef struct col_info_s {
    text_ref_t  name_ref;
    text_ref_t  format_ref;
    text_ref_t  label_ref;

    int    index;
    int    offset;
    int    width;
    int    type;
} col_info_t;

typedef struct sas_header_info_s {
    int      little_endian;
    int      u64;
    int      page_size;
    int      page_count;
} sas_header_info_t;

typedef struct sas_catalog_ctx_s {
    readstat_handle_value_label_callback  value_label_cb;
    void         *user_ctx;
    int           u64;
    int           bswap;
} sas_catalog_ctx_t;

typedef struct sas_ctx_s {
    readstat_handle_info_callback      info_cb;
    readstat_handle_variable_callback  variable_cb;
    readstat_handle_value_callback    value_cb;
    int           little_endian;
    int           u64;
    void         *user_ctx;
    int           bswap;
    int           did_submit_columns;
    int32_t       row_length;
    int32_t       page_row_count;
    int32_t       parsed_row_count;
    int32_t       total_row_count;
    int32_t       column_count;
    int           text_blob_count;
    size_t       *text_blob_lengths;
    char        **text_blobs;

    int           col_names_count;
    int           col_attrs_count;
    int           col_formats_count;

    int           max_col_width;
    char         *scratch_buffer;

    int           col_info_count;
    col_info_t   *col_info;
} sas_ctx_t;

typedef struct cat_ctx_s {
    readstat_handle_value_label_callback  value_label_cb;
    int        u64;
    void      *user_ctx;
    int32_t    header_size;
    int32_t    page_size;
    int32_t    page_count;
} cat_ctx_t;

static void unpad(char *string, size_t len) {
    string[len] = '\0';
    /* remove space padding */
    size_t i;
    for (i=len-1; i>0; i--) {
        if (string[i] == ' ') {
            string[i] = '\0';
        } else {
            break;
        }
    }
}

static uint64_t read8(const char *data, int bswap) {
    uint64_t tmp;
    memcpy(&tmp, data, 8);
    return bswap ? byteswap8(tmp) : tmp;
}

static uint32_t read4(const char *data, int bswap) {
    uint32_t tmp;
    memcpy(&tmp, data, 4);
    return bswap ? byteswap4(tmp) : tmp;
}

static uint16_t read2(const char *data, int bswap) {
    uint16_t tmp;
    memcpy(&tmp, data, 2);
    return bswap ? byteswap2(tmp) : tmp;
}

static void sas_ctx_free(sas_ctx_t *ctx) {
    int i;
    if (ctx->text_blobs) {
        for (i=0; i<ctx->text_blob_count; i++) {
            free(ctx->text_blobs[i]);
        }
        free(ctx->text_blobs);
        free(ctx->text_blob_lengths);
    }
    if (ctx->col_info)
        free(ctx->col_info);

    if (ctx->scratch_buffer)
        free(ctx->scratch_buffer);

    free(ctx);
}

static readstat_errors_t sas_read_header(int fd, sas_header_info_t *ctx) {
    sas_header_start_t  header_start;
    sas_header_end_t    header_end;
    int retval = 0;
    size_t a1 = 0, a2 = 0;
    if (read(fd, &header_start, sizeof(sas_header_start_t)) < sizeof(sas_header_start_t)) {
        retval = READSTAT_ERROR_READ;
        goto cleanup;
    }
    if (memcmp(header_start.magic, sas7bdat_magic_number, sizeof(sas7bdat_magic_number)) != 0 &&
            memcmp(header_start.magic, sas7bcat_magic_number, sizeof(sas7bdat_magic_number)) != 0) {
        retval = READSTAT_ERROR_PARSE;
        goto cleanup;
    }
    if (header_start.a1 == SAS_ALIGNMENT_OFFSET_4) {
        a1 = 4;
    }
    if (header_start.a2 == SAS_ALIGNMENT_OFFSET_4) {
        ctx->u64 = 1;
        a2 = 4;
    }
    int bswap = 0;
    if (header_start.endian == SAS_ENDIAN_BIG) {
        bswap = machine_is_little_endian();
        ctx->little_endian = 0;
    } else if (header_start.endian == SAS_ENDIAN_LITTLE) {
        bswap = !machine_is_little_endian();
        ctx->little_endian = 1;
    } else {
        retval = READSTAT_ERROR_PARSE;
        goto cleanup;
    }
    if (lseek(fd, 196 + a1, SEEK_SET) == -1) {
        retval = READSTAT_ERROR_READ;
        goto cleanup;
    }

    uint32_t header_size, page_size;

    if (read(fd, &header_size, sizeof(uint32_t)) < sizeof(uint32_t)) {
        retval = READSTAT_ERROR_READ;
        goto cleanup;
    }
    if (read(fd, &page_size, sizeof(uint32_t)) < sizeof(uint32_t)) {
        retval = READSTAT_ERROR_READ;
        goto cleanup;
    }

    header_size = bswap ? byteswap4(header_size) : header_size;

    ctx->page_size = bswap ? byteswap4(page_size) : page_size;

    if (header_size != 8192 && header_size != 1024) {
        retval = READSTAT_ERROR_PARSE;
        goto cleanup;
    }

    if (ctx->u64) {
        uint64_t page_count;
        if (read(fd, &page_count, sizeof(uint64_t)) < sizeof(uint64_t)) {
            retval = READSTAT_ERROR_READ;
            goto cleanup;
        }
        ctx->page_count = bswap ? byteswap8(page_count) : page_count;
    } else {
        uint32_t page_count;
        if (read(fd, &page_count, sizeof(uint32_t)) < sizeof(uint32_t)) {
            retval = READSTAT_ERROR_READ;
            goto cleanup;
        }
        ctx->page_count = bswap ? byteswap4(page_count) : page_count;
    }
    
    if (lseek(fd, 8 + a1, SEEK_CUR) == -1) {
        retval = READSTAT_ERROR_READ;
        goto cleanup;
    }
    if (read(fd, &header_end, sizeof(sas_header_end_t)) < sizeof(sas_header_end_t)) {
        retval = READSTAT_ERROR_READ;
        goto cleanup;
    }
    if (lseek(fd, header_size, SEEK_SET) == -1) {
        retval = READSTAT_ERROR_READ;
        goto cleanup;
    }

cleanup:
    return retval;
}

static readstat_errors_t sas_parse_column_text_subheader(const char *subheader, size_t len, sas_ctx_t *ctx) {
    readstat_errors_t retval = 0;
    size_t signature_len = ctx->u64 ? 8 : 4;
    uint16_t remainder = read2(&subheader[signature_len], ctx->bswap);
    if (remainder != len - (4+2*signature_len)) {
        retval = READSTAT_ERROR_PARSE;
        goto cleanup;
    }
    ctx->text_blob_count++;
    ctx->text_blobs = realloc(ctx->text_blobs, ctx->text_blob_count * sizeof(char *));
    ctx->text_blob_lengths = realloc(ctx->text_blob_lengths,
            ctx->text_blob_count * sizeof(ctx->text_blob_lengths[0]));
    char *blob = malloc(len-signature_len);
    memcpy(blob, subheader+signature_len, len-signature_len);
    ctx->text_blob_lengths[ctx->text_blob_count-1] = len-signature_len;
    ctx->text_blobs[ctx->text_blob_count-1] = blob;

cleanup:
    return retval;
}

static readstat_errors_t sas_parse_column_size_subheader(const char *subheader, size_t len, sas_ctx_t *ctx) {
    readstat_errors_t retval = 0;

    uint64_t col_count;

    if (ctx->u64) {
        col_count = read8(&subheader[8], ctx->bswap);
    } else {
        col_count = read4(&subheader[4], ctx->bswap);
    }

    ctx->column_count = col_count;

    return retval;
}

static readstat_errors_t sas_parse_row_size_subheader(const char *subheader, size_t len, sas_ctx_t *ctx) {
    readstat_errors_t retval = 0;
    uint64_t total_row_count;
    uint64_t row_length, page_row_count;

    if (ctx->u64) {
        row_length = read8(&subheader[40], ctx->bswap);
        total_row_count = read8(&subheader[48], ctx->bswap);
        page_row_count = read8(&subheader[120], ctx->bswap);
    } else {
        row_length = read4(&subheader[20], ctx->bswap);
        total_row_count = read4(&subheader[24], ctx->bswap);
        page_row_count = read4(&subheader[60], ctx->bswap);
    }

    ctx->row_length = row_length;
    ctx->page_row_count = page_row_count;
    ctx->total_row_count = total_row_count;

    return retval;
}

static text_ref_t sas_parse_text_ref(const char *data, sas_ctx_t *ctx) {
    text_ref_t  ref;

    ref.index = read2(&data[0], ctx->bswap);
    ref.offset = read2(&data[2], ctx->bswap);
    ref.length = read2(&data[4], ctx->bswap);

    return ref;
}

static readstat_errors_t copy_text_ref(char *out_buffer, text_ref_t text_ref, size_t len, sas_ctx_t *ctx) {
    if (text_ref.index < 0 || text_ref.index >= ctx->text_blob_count)
        return READSTAT_ERROR_PARSE;
    
    if (text_ref.length == 0) {
        out_buffer[0] = '\0';
        return 0;
    }

    char *blob = ctx->text_blobs[text_ref.index];

    if (text_ref.offset < 0 || text_ref.length < 0 ||
            text_ref.offset + text_ref.length > ctx->text_blob_lengths[text_ref.index])
        return READSTAT_ERROR_PARSE;

    size_t out_len = len-1;
    if (text_ref.length < out_len)
        out_len = text_ref.length;

    memcpy(out_buffer, &blob[text_ref.offset], out_len);
    out_buffer[out_len] = '\0';

    return 0;
}

static readstat_errors_t sas_parse_column_name_subheader(const char *subheader, size_t len, sas_ctx_t *ctx) {
    readstat_errors_t retval = 0;
    size_t signature_len = ctx->u64 ? 8 : 4;
    int cmax = ctx->u64 ? (len-28)/8 : (len-20)/8;
    int i;
    const char *cnp = &subheader[signature_len+8];
    uint16_t remainder = read2(&subheader[signature_len], ctx->bswap);

    if (remainder != len - (4+2*signature_len)) {
        retval = READSTAT_ERROR_PARSE;
        goto cleanup;
    }

    ctx->col_names_count += cmax;
    if (ctx->col_info_count < ctx->col_names_count) {
        ctx->col_info_count = ctx->col_names_count;
        ctx->col_info = realloc(ctx->col_info, ctx->col_info_count * sizeof(col_info_t));
    }
    for (i=ctx->col_names_count-cmax; i<ctx->col_names_count; i++) {
        ctx->col_info[i].name_ref = sas_parse_text_ref(cnp, ctx);
        cnp += 8;
    }

cleanup:

    return retval;
}

static readstat_errors_t sas_parse_column_attributes_subheader(const char *subheader, size_t len, sas_ctx_t *ctx) {
    readstat_errors_t retval = 0;
    size_t signature_len = ctx->u64 ? 8 : 4;
    int cmax = ctx->u64 ? (len-28)/16 : (len-20)/12;
    int i;
    const char *cap = &subheader[signature_len+8];
    uint16_t remainder = read2(&subheader[signature_len], ctx->bswap);

    if (remainder != len - (4+2*signature_len)) {
        retval = READSTAT_ERROR_PARSE;
        goto cleanup;
    }
    ctx->col_attrs_count += cmax;
    if (ctx->col_info_count < ctx->col_attrs_count) {
        ctx->col_info_count = ctx->col_attrs_count;
        ctx->col_info = realloc(ctx->col_info, ctx->col_info_count * sizeof(col_info_t));
    }
    for (i=ctx->col_attrs_count-cmax; i<ctx->col_attrs_count; i++) {
        if (ctx->u64) {
            ctx->col_info[i].offset = read8(&cap[0], ctx->bswap);
        } else {
            ctx->col_info[i].offset = read4(&cap[0], ctx->bswap);
        }

        off_t off=4;
        if (ctx->u64)
            off=8;

        ctx->col_info[i].width = read4(&cap[off], ctx->bswap);
        if (ctx->col_info[i].width > ctx->max_col_width)
            ctx->max_col_width = ctx->col_info[i].width;

        if (cap[off+6] == SAS_COLUMN_TYPE_NUM) {
            ctx->col_info[i].type = READSTAT_TYPE_DOUBLE;
        } else if (cap[off+6] == SAS_COLUMN_TYPE_CHR) {
            ctx->col_info[i].type = READSTAT_TYPE_STRING;
        } else {
            retval = READSTAT_ERROR_PARSE;
            goto cleanup;
        }
        ctx->col_info[i].index = i;
        cap += off+8;
    }

cleanup:

    return retval;
}

static readstat_errors_t sas_parse_column_format_subheader(const char *subheader, size_t len, sas_ctx_t *ctx) {
    readstat_errors_t retval = 0;

    ctx->col_formats_count++;
    if (ctx->col_info_count < ctx->col_formats_count) {
        ctx->col_info_count = ctx->col_formats_count;
    }

    ctx->col_info[ctx->col_formats_count-1].format_ref = sas_parse_text_ref(
            ctx->u64 ? &subheader[46] : &subheader[34], ctx);
    ctx->col_info[ctx->col_formats_count-1].label_ref = sas_parse_text_ref(
            ctx->u64 ? &subheader[52] : &subheader[40], ctx);

    return retval;
}

static int handle_data_value(const unsigned char *col_data, col_info_t *col_info, sas_ctx_t *ctx) {
    int cb_retval = 0;
    if (col_info->type == READSTAT_TYPE_STRING) {
        memcpy(ctx->scratch_buffer, col_data, col_info->width);
        unpad(ctx->scratch_buffer, col_info->width);
        cb_retval = ctx->value_cb(ctx->parsed_row_count, col_info->index, 
                ctx->scratch_buffer, READSTAT_TYPE_STRING, ctx->user_ctx);
    } else if (col_info->type == READSTAT_TYPE_DOUBLE) {
        uint64_t  val = 0;
        if (ctx->little_endian) {
            int k;
            for (k=0; k<col_info->width; k++) {
                val = (val << 8) | col_data[col_info->width-1-k];
            }
        } else {
            int k;
            for (k=0; k<col_info->width; k++) {
                val = (val << 8) | col_data[k];
            }
        }
        val <<= (8-col_info->width)*8;

        cb_retval = ctx->value_cb(ctx->parsed_row_count, col_info->index, 
                (double *)&val, READSTAT_TYPE_DOUBLE, ctx->user_ctx);
    }
    return cb_retval;
}

static readstat_errors_t sas_parse_single_row(const char *data, sas_ctx_t *ctx) {
    readstat_errors_t retval = 0;
    int j;
    ctx->scratch_buffer = realloc(ctx->scratch_buffer, ctx->max_col_width + 1);
    for (j=0; j<ctx->column_count; j++) {
        col_info_t *col_info = &ctx->col_info[j];
        int cb_retval = handle_data_value((const unsigned char *)&data[col_info->offset],
                col_info, ctx);
        if (cb_retval) {
            retval = READSTAT_ERROR_USER_ABORT;
            goto cleanup;
        }
    }
    ctx->parsed_row_count++;

cleanup:
    return retval;
}

static readstat_errors_t sas_parse_rows(const char *data, sas_ctx_t *ctx) {
    readstat_errors_t retval = 0;
    int i;
    size_t row_offset=0;
    for (i=0; i<ctx->page_row_count && ctx->parsed_row_count < ctx->total_row_count; i++) {
        if ((retval = sas_parse_single_row(&data[row_offset], ctx)) != 0)
            goto cleanup;

        row_offset += ctx->row_length;
    }

cleanup:
    return retval;
}

static readstat_errors_t sas_parse_subheader_rle(const char *subheader, size_t len, sas_ctx_t *ctx) {
    /* TODO bounds checking */
    readstat_errors_t retval = 0;
    const char *input = subheader;
    char *buffer = malloc(ctx->row_length);
    char *output = buffer;
    while (input < subheader + len) {
        unsigned char control = input[0];
        unsigned char command = (control & 0xF0) >> 4;
        unsigned char length = (control & 0x0F);
        int copy_len = 0;
        int insert_len = 0;
        char insert_byte = '\0';
        input++;
        switch (command) {
            case SAS_RLE_COMMAND_COPY64:
                copy_len = (unsigned char)input[0] + 64;
                input++;
                break;
            case SAS_RLE_COMMAND_INSERT_BLANK17:
                insert_len = (unsigned char)input[0] + 17;
                insert_byte = ' ';
                input++;
                break;
            case SAS_RLE_COMMAND_INSERT_ZERO17:
                insert_len = (unsigned char)input[0] + 17;
                insert_byte = '\0';
                input++;
                break;
            case SAS_RLE_COMMAND_COPY1:  copy_len = length + 1; break;
            case SAS_RLE_COMMAND_COPY17: copy_len = length + 17; break;
            case SAS_RLE_COMMAND_COPY33: copy_len = length + 33; break;
            case SAS_RLE_COMMAND_COPY49: copy_len = length + 49; break;
            case SAS_RLE_COMMAND_INSERT_BYTE3:
                insert_byte = input[0];
                insert_len = length + 3;
                input++;
                break;
            case SAS_RLE_COMMAND_INSERT_AT2:
                insert_byte = '@';
                insert_len = length + 2;
                break;
            case SAS_RLE_COMMAND_INSERT_BLANK2:
                insert_byte = ' ';
                insert_len = length + 2;
                break;
            case SAS_RLE_COMMAND_INSERT_ZERO2:
                insert_byte = '\0';
                insert_len = length + 2;
                break;
            default:
                retval = READSTAT_ERROR_PARSE;
                goto cleanup;
        }
        if (copy_len) {
            memcpy(output, input, copy_len);
            input += copy_len;
            output += copy_len;
        }
        if (insert_len) {
            memset(output, insert_byte, insert_len);
            output += insert_len;
        }
    }
    if (output - buffer != ctx->row_length) {
        retval = READSTAT_ERROR_PARSE;
        goto cleanup;
    }
    retval = sas_parse_single_row(buffer, ctx);
cleanup:
    free(buffer);

    return retval;
}

static readstat_errors_t sas_parse_subheader(const char *subheader, size_t len, sas_ctx_t *ctx) {
    readstat_errors_t retval = 0;

    if (len < 6) {
        retval = READSTAT_ERROR_PARSE;
        goto cleanup;
    }
    uint32_t signature = read4(subheader, ctx->bswap);

    if (signature == SAS_SUBHEADER_SIGNATURE_ROW_SIZE) {
        retval = sas_parse_row_size_subheader(subheader, len, ctx);
    } else if (signature == SAS_SUBHEADER_SIGNATURE_COLUMN_SIZE) {
        retval = sas_parse_column_size_subheader(subheader, len, ctx);
    } else {
        if (!ctx->little_endian && signature == -1 && ctx->u64) {
            signature = read4(&subheader[4], ctx->bswap);
        }
        if (signature == SAS_SUBHEADER_SIGNATURE_COUNTS) {
        } else if (signature == SAS_SUBHEADER_SIGNATURE_COLUMN_TEXT) {
            retval = sas_parse_column_text_subheader(subheader, len, ctx);
        } else if (signature == SAS_SUBHEADER_SIGNATURE_COLUMN_NAME) {
            retval = sas_parse_column_name_subheader(subheader, len, ctx);
        } else if (signature == SAS_SUBHEADER_SIGNATURE_COLUMN_ATTRS) {
            retval = sas_parse_column_attributes_subheader(subheader, len, ctx);
        } else if (signature == SAS_SUBHEADER_SIGNATURE_COLUMN_FORMAT) {
            retval = sas_parse_column_format_subheader(subheader, len, ctx);
        } else if (signature == SAS_SUBHEADER_SIGNATURE_COLUMN_LIST) {
        } else {
            retval = READSTAT_ERROR_PARSE;
        }
    }

cleanup:

    return retval;
}

static readstat_errors_t sas_parse_catalog_page(const char *page, size_t page_size, sas_catalog_ctx_t *ctx) {
    readstat_errors_t retval = 0;
    if (ctx->u64)
        retval = READSTAT_ERROR_PARSE;

    int i;
    for (i=16; i<22; i++) {
        if (page[i]) {
            /* not a labels page... I think */
            goto cleanup;
        }
    }

    const char *lsp = &page[22];

    while (lsp < page + page_size) {
        size_t block_size = 255 * (1+lsp[9]);
        int label_count1 = read4(&lsp[48], ctx->bswap);
        int label_count2 = read4(&lsp[52], ctx->bswap);
        char name[9];

        memcpy(name, &lsp[18], 8);
        unpad(name, 8);
        
        int is_string = 0;
        size_t md_len = 46;
        if (name[0] == '$') {
            md_len = 30;
            is_string = 1;
        }

        const char *lbp1 = &lsp[116];
        const char *lbp2 = &lsp[116+label_count1*md_len];

        for (i=0; i<label_count2; i++) {
            size_t len = read2(&lbp2[8], ctx->bswap);
            const char *label = &lbp2[10];
            if (is_string) {
                char val[17];
                memcpy(val, &lbp1[14], 16);
                unpad(val, 16);
                if (ctx->value_label_cb)
                    ctx->value_label_cb(name, val, READSTAT_TYPE_STRING, label, ctx->user_ctx);
            } else {
                uint64_t val = read8(&lbp1[22], 1);
                double dval;
                memcpy(&dval, &val, 8);
                dval *= -1.0;
                if (ctx->value_label_cb)
                    ctx->value_label_cb(name, &val, READSTAT_TYPE_DOUBLE, label, ctx->user_ctx);
            }

            lbp1 += md_len;
            lbp2 += 8 + 2 + len + 1;
        }

        lsp += block_size;
    }

cleanup:
    return retval;
}

static readstat_errors_t submit_columns(sas_ctx_t *ctx) {
    readstat_errors_t retval = 0;
    if (ctx->info_cb) {
        if (ctx->info_cb(ctx->total_row_count, ctx->column_count, ctx->user_ctx)) {
            retval = READSTAT_ERROR_USER_ABORT;
            goto cleanup;
        }
    }
    if (ctx->variable_cb) {
        int i;
        char name_buf[1024];
        char format_buf[1024];
        char label_buf[1024];
        for (i=0; i<ctx->column_count; i++) {
            if ((retval = copy_text_ref(name_buf, ctx->col_info[i].name_ref, sizeof(name_buf), ctx)) != 0) {
                goto cleanup;
            }
            if ((retval = copy_text_ref(format_buf, ctx->col_info[i].format_ref, sizeof(format_buf), ctx)) != 0) {
                goto cleanup;
            }
            if ((retval = copy_text_ref(label_buf, ctx->col_info[i].label_ref, sizeof(label_buf), ctx)) != 0) {
                goto cleanup;
            }
            char *stata_format = NULL;
            char *label_set = NULL;
            if (strcmp(format_buf, "DATE") == 0) {
                stata_format = "%td";
            } else if (strcmp(format_buf, "DATETIME") == 0) {
                stata_format = "%ts";
            } else if (format_buf[0] != '\0') {
                label_set = format_buf;
                // dprintf(STDERR_FILENO,  "Unknown format: %s\n", format_buf);
            }
            int cb_retval = ctx->variable_cb(i, name_buf, stata_format, label_buf, label_set, ctx->col_info[i].type,
                    ctx->col_info[i].type == READSTAT_TYPE_DOUBLE ? 8 : ctx->col_info[i].width, 
                    ctx->user_ctx);
            if (cb_retval) {
                retval = READSTAT_ERROR_USER_ABORT;
                goto cleanup;
            }
        }
    }
cleanup:
    return retval;
}

static readstat_errors_t sas_parse_page(const char *page, size_t page_size, sas_ctx_t *ctx) {
    uint16_t page_type;

    readstat_errors_t retval = 0;

    off_t off = 0;
    if (ctx->u64)
        off = 16;

    page_type = read2(&page[off+16], ctx->bswap);

    const char *data = NULL;

    if ((page_type & SAS_PAGE_TYPE_MASK) == SAS_PAGE_TYPE_DATA) {
        ctx->page_row_count = read2(&page[off+18], ctx->bswap);
        data = &page[off+24];
    } else { 
        uint16_t subheader_count = read2(&page[off+20], ctx->bswap);

        int i;
        const char *shp = &page[off+24];
        for (i=0; i<subheader_count; i++) {
            uint64_t offset = 0, len = 0;
            unsigned char compression = 0;
            unsigned char subheader_type = 0;
            int lshp = 0;
            if (ctx->u64) {
                offset = read8(&shp[0], ctx->bswap);
                len = read8(&shp[8], ctx->bswap);
                compression = shp[16];
                subheader_type = shp[17];
                lshp = 24;
            } else {
                offset = read4(&shp[0], ctx->bswap);
                len = read4(&shp[4], ctx->bswap);
                compression = shp[8];
                subheader_type = shp[9];
                lshp = 12;
            }

            if (len > 0 && compression != SAS_COMPRESSION_TRUNC) {
                if (offset > page_size || offset + len > page_size ||
                    offset < off+24+subheader_count*lshp) {
                    retval = READSTAT_ERROR_PARSE;
                    goto cleanup;
                }
                if (compression == SAS_COMPRESSION_NONE) {
                    if ((retval = sas_parse_subheader(page + offset, len, ctx)) != 0) {
                        goto cleanup;
                    }
                } else if (compression == SAS_COMPRESSION_RLE) {
                    if (!ctx->did_submit_columns) {
                        if ((retval = submit_columns(ctx)) != 0) {
                            goto cleanup;
                        }
                        ctx->did_submit_columns = 1;
                    }
                    if ((retval = sas_parse_subheader_rle(page + offset, len, ctx)) != 0) {
                        goto cleanup;
                    }
                } else {
                    retval = READSTAT_ERROR_UNSUPPORTED_COMPRESSION;
                    goto cleanup;
                }
            }

            shp += lshp;
        }

        if ((page_type & SAS_PAGE_TYPE_MASK) == SAS_PAGE_TYPE_MIX) {
            data = shp + ((shp-page)%8);
        }
    }
    if (data) {
        if (!ctx->did_submit_columns) {
            if ((retval = submit_columns(ctx)) != 0) {
                goto cleanup;
            }
            ctx->did_submit_columns = 1;
        }
        if (ctx->value_cb) {
            retval = sas_parse_rows(data, ctx);
        }
    } 

cleanup:

    return retval;
}

int parse_sas7bdat(const char *filename, void *user_ctx,
        readstat_handle_info_callback info_cb, 
        readstat_handle_variable_callback variable_cb,
        readstat_handle_value_callback value_cb) {
    int fd = -1;
    readstat_errors_t retval = 0;

    sas_ctx_t  *ctx = calloc(1, sizeof(sas_ctx_t));
    sas_header_info_t  *hinfo = calloc(1, sizeof(sas_header_info_t));

    ctx->info_cb = info_cb;
    ctx->variable_cb = variable_cb;
    ctx->value_cb = value_cb;
    ctx->user_ctx = user_ctx;

    if ((fd = readstat_open(filename)) == -1) {
        retval = READSTAT_ERROR_OPEN;
        goto cleanup;
    }

    if ((retval = sas_read_header(fd, hinfo)) != 0) {
        goto cleanup;
    }

    ctx->u64 = hinfo->u64;
    ctx->little_endian = hinfo->little_endian;
    ctx->bswap = machine_is_little_endian() ^ hinfo->little_endian;

    int i;
    char *page = malloc(hinfo->page_size);
    for (i=0; i<hinfo->page_count; i++) {
        if (read(fd, page, hinfo->page_size) < hinfo->page_size) {
            retval = READSTAT_ERROR_READ;
            goto cleanup;
        }
        
        if ((retval = sas_parse_page(page, hinfo->page_size, ctx)) != 0) {
            goto cleanup;
        }
    }
    free(page);
    
    if (!ctx->did_submit_columns) {
        if ((retval = submit_columns(ctx)) != 0) {
            goto cleanup;
        }
        ctx->did_submit_columns = 1;
    }

    if (ctx->parsed_row_count != ctx->total_row_count) {
        retval = READSTAT_ERROR_ROW_COUNT_MISMATCH;
        dprintf(STDERR_FILENO, "ReadStat: Expected %d rows in file, found %d\n",
                ctx->total_row_count, ctx->parsed_row_count);
        goto cleanup;
    }

    char test;
    if (read(fd, &test, 1) == 1) {
        retval = READSTAT_ERROR_PARSE;
        goto cleanup;
    }

cleanup:
    if (ctx)
        sas_ctx_free(ctx);
    if (fd != -1)
        readstat_close(fd);
    if (hinfo)
        free(hinfo);

    return retval;
}

int parse_sas7bcat(const char *filename, void *user_ctx,
        readstat_handle_value_label_callback value_label_cb) {
    int fd = -1;
    readstat_errors_t retval = 0;

    sas_catalog_ctx_t *ctx = calloc(1, sizeof(sas_catalog_ctx_t));
    sas_header_info_t *hinfo = calloc(1, sizeof(sas_header_info_t));

    ctx->value_label_cb = value_label_cb;
    ctx->user_ctx = user_ctx;

    if ((fd = readstat_open(filename)) == -1) {
        retval = READSTAT_ERROR_OPEN;
        goto cleanup;
    }

    if ((retval = sas_read_header(fd, hinfo)) != 0) {
        goto cleanup;
    }

    ctx->u64 = hinfo->u64;
    ctx->bswap = machine_is_little_endian() ^ hinfo->little_endian;

    int i;
    char *page = malloc(hinfo->page_size);
    for (i=0; i<hinfo->page_count; i++) {
        if (read(fd, page, hinfo->page_size) < hinfo->page_size) {
            retval = READSTAT_ERROR_READ;
            goto cleanup;
        }
        
        /* skip the first three pages cuz they suck */
        if (i < 3)
            continue;

        if ((retval = sas_parse_catalog_page(page, hinfo->page_size, ctx)) != 0) {
            goto cleanup;
        }
    }
    free(page);

    char test;
    if (read(fd, &test, 1) == 1) {
        retval = READSTAT_ERROR_PARSE;
        goto cleanup;
    }

cleanup:
    if (ctx)
        free(ctx);
    if (fd != -1)
        readstat_close(fd);
    if (hinfo)
        free(hinfo);

    return retval;
}
