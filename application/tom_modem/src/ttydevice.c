#include "ttydevice.h"
static int tty_set_device(PROFILE_T *profile, FDS_T *fds)
{
    int baud_rate, data_bits;
    struct termios tty;
    baud_rate = profile->baud_rate;
    data_bits = profile->data_bits;
    if (tcgetattr(fds->tty_fd, &tty) != 0)
    {
        err_msg("Error getting tty attributes");
        return COMM_ERROR;
    }
    memmove(&fds->old_termios, &tty, sizeof(struct termios));
    cfmakeraw(&tty);
    tty.c_cflag |= CLOCAL; // 忽略调制解调器控制线，允许本地连接
    tty.c_cflag |= CREAD;  // 使能接收

    // clear flow control ,stop bits parity
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~PARENB;
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;

    // set data bits 5,6,7,8
    tty.c_cflag &= ~CSIZE; // 清除数据位设置
    switch (data_bits)
    {
    case 5:
        tty.c_cflag |= CS5;
        break;
    case 6:
        tty.c_cflag |= CS6;
        break;
    case 7:
        tty.c_cflag |= CS7;
        break;
    case 8:
        tty.c_cflag |= CS8;
        break;
    default:
        tty.c_cflag |= CS8;
        break;
    }

    // set baud rate
    switch (baud_rate)
    {
    case 4800:
        cfsetspeed(&tty, B4800);
        break;
    case 9600:
        cfsetspeed(&tty, B9600);
        break;
    case 19200:
        cfsetspeed(&tty, B19200);
        break;
    case 38400:
        cfsetspeed(&tty, B38400);
        break;
    case 57600:
        cfsetspeed(&tty, B57600);
        break;
    case 115200:
        cfsetspeed(&tty, B115200);
        break;

    default:
        cfsetspeed(&tty, B115200);
        break;
    }
    if (tcsetattr(fds->tty_fd, TCSANOW, &tty) != 0)
    {
        err_msg("Error setting tty attributes");
        return COMM_ERROR;
    }
    return SUCCESS;
}
// Helper function to comprehensively clear all buffers
static int tty_clear_kernel_buffers(int fd, FILE *fdi, FILE *fdo)
{
    int result = SUCCESS;
    
    // Clear stdio buffers if file streams exist
    if (fdi != NULL) {
        if (fflush(fdi) != 0) {
            err_msg("Failed to flush input stream: %s", strerror(errno));
            result = COMM_ERROR;
        }
    }
    
    if (fdo != NULL) {
        if (fflush(fdo) != 0) {
            err_msg("Failed to flush output stream: %s", strerror(errno));
            result = COMM_ERROR;
        }
    }
    
    // Clear kernel TTY buffers
    if (fd >= 0) {
        if (tcflush(fd, TCIOFLUSH) != 0) {
            err_msg("Failed to flush TTY kernel buffers: %s", strerror(errno));
            result = COMM_ERROR;
        } else {
            dbg_msg("TTY buffers cleared successfully");
        }
    }
    
    return result;
}

static int clear_modem_side_buffers(FILE *fdi) {
    if (!fdi) {
        err_msg("Invalid file stream pointer");
        return COMM_ERROR;
    }

    int fd = fileno(fdi);
    if (fd < 0) {
        err_msg("Invalid file descriptor from stream");
        return COMM_ERROR;
    }

    if (tty_clear_kernel_buffers(fd,NULL,NULL) != SUCCESS) {
        err_msg("Failed to flush TTY kernel buffers");
        return COMM_ERROR;
    }

    // Then, drain any remaining data from modem side buffers
    char drain_buffer[256];
    int bytes_read = 0;
    int total_bytes_drained = 0;
    time_t start_time = time(NULL);
    const double DRAIN_TIMEOUT = 1; // 1 second timeout
    
    // Drain loop
    while (difftime(time(NULL), start_time) < DRAIN_TIMEOUT) {
        if (fgets(drain_buffer, sizeof(drain_buffer), fdi) != NULL) {
            bytes_read = strlen(drain_buffer);
            total_bytes_drained += bytes_read;
            dbg_msg("Drained %d bytes from modem buffer: %.50s%s", 
                   bytes_read, drain_buffer, 
                   (bytes_read > 50) ? "..." : "");
        } else {
            break;
        }
    }

    if (tty_clear_kernel_buffers(fd,NULL,NULL) != SUCCESS) {
        err_msg("Warning: Failed final TTY buffer flush");

    }

    if (total_bytes_drained > 0) {
        dbg_msg("Successfully drained %d bytes from modem buffer in %.2f seconds", 
               total_bytes_drained, difftime(time(NULL), start_time));
    } else {
        dbg_msg("No data found in modem buffer (buffer was already clean)");
    }
    return SUCCESS;
}


int tty_open_device(PROFILE_T *profile,FDS_T *fds)
{
    // Initialize file descriptors to invalid values
    fds->tty_fd = -1;
    fds->fdi = NULL;
    fds->fdo = NULL;
    
    // Step 1: Open device for initial configuration
    fds->tty_fd = open(profile->tty_dev, O_RDWR | O_NOCTTY);
    if (fds->tty_fd < 0)
    {
        err_msg("Error opening tty device: %s", profile->tty_dev);
        return COMM_ERROR;
    }

    if (tty_set_device(profile,fds) != 0)
    {
        err_msg("Error setting tty device");
        close(fds->tty_fd);
        return COMM_ERROR;
    }

    // Step 2: Close and reopen with non-blocking mode for stdio streams
    close(fds->tty_fd);
    fds->tty_fd = open(profile->tty_dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fds->tty_fd < 0)
    {
        err_msg("Error reopening tty device in non-blocking mode: %s", profile->tty_dev);
        return COMM_ERROR;
    }

    // Clear buffers after reopening
    dbg_msg("Clearing buffers after device reopen");
    tty_clear_kernel_buffers(fds->tty_fd, NULL, NULL);

    // Step 3: Create stdio streams
    fds->fdi = fdopen(fds->tty_fd, "r");
    fds->fdo = fdopen(fds->tty_fd, "w");
    if (fds->fdi == NULL || fds->fdo == NULL)
    {
        err_msg("Error opening file descriptor");
        if (fds->fdi) fclose(fds->fdi);
        if (fds->fdo) fclose(fds->fdo);
        if (fds->tty_fd >= 0) close(fds->tty_fd);
        return COMM_ERROR;
    }

    // Step 4: Configure buffering for output stream
    if (setvbuf(fds->fdo , NULL, _IOFBF, 0))
    {
        err_msg("Error setting buffer for fdo");
        fclose(fds->fdi);
        fclose(fds->fdo);
        return COMM_ERROR;
    }

    // Step 5: Configure buffering for input stream
    if (setvbuf(fds->fdi , NULL, _IOLBF, 0))
    {
        err_msg("Error setting buffer for fdi");
        fclose(fds->fdi);
        fclose(fds->fdo);
        return COMM_ERROR;
    }

    usleep(5000);


    if (profile->clear_modem_side_buffers)
    {
        // Clear modem side buffers if requested
        dbg_msg("Clearing modem side buffers");
        if (clear_modem_side_buffers(fds->fdi) != SUCCESS)
        {
            err_msg("Failed to clear modem side buffers");
            fclose(fds->fdi);
            fclose(fds->fdo);
            close(fds->tty_fd);
            return COMM_ERROR;
        }
    }
    else 
    {
        if (tty_clear_kernel_buffers(fds->tty_fd, fds->fdi, fds->fdo) != SUCCESS)
        {
            err_msg("Warning: Failed to clear some buffers during final cleanup");
        }
    }
    return SUCCESS;
}

int tty_read(FILE *fdi, AT_MESSAGE_T *message, PROFILE_T *profile) {
    return tty_read_keyword(fdi, message, NULL, profile);
}

static int is_at_termination_line(const char *line, const char *key_word) {
    if (!line || strlen(line) == 0) {
        return 0;
    }

    // Check standard AT command responses
    if (strncmp(line, "OK", 2) == 0 ||
        strncmp(line, "ERROR", 5) == 0 ||
        strncmp(line, "+CMS ERROR:", 11) == 0 ||
        strncmp(line, "+CME ERROR:", 11) == 0 ||
        strncmp(line, "NO CARRIER", 10) == 0) {
        return 1;
    }

    // Check for specific keyword if provided
    if (key_word != NULL && strncmp(line, key_word, strlen(key_word)) == 0) {
        return 2; // Special return value for keyword match
    }

    return 0;
}

static int append_to_buffer(char **dynamic_buffer, int *buffer_size, const char *line) {
    if (!line || !dynamic_buffer || !buffer_size) {
        return COMM_ERROR;
    }

    int line_len = strlen(line);
    if (line_len == 0) {
        return SUCCESS;
    }

    const int MAX_BUFFER_SIZE = 64 * 1024;
    if (*buffer_size + line_len + 1 > MAX_BUFFER_SIZE) {
        err_msg("Buffer size would exceed maximum limit (%d bytes)", MAX_BUFFER_SIZE);
        return BUFFER_OVERFLOW;
    }

    char *new_buffer = realloc(*dynamic_buffer, *buffer_size + line_len + 1);
    if (!new_buffer) {
        err_msg("Memory allocation failed for buffer size %d", *buffer_size + line_len + 1);
        return BUFFER_OVERFLOW;
    }

    *dynamic_buffer = new_buffer;
    memcpy(*dynamic_buffer + *buffer_size, line, line_len);
    *buffer_size += line_len;
    (*dynamic_buffer)[*buffer_size] = '\0';

    return SUCCESS;
}

int tty_read_keyword(FILE *fdi, AT_MESSAGE_T *message, char *key_word, PROFILE_T *profile) {
    // Input validation
    if (!fdi || !profile) {
        err_msg("Invalid parameters: fdi or profile is NULL");
        return COMM_ERROR;
    }

    char line_buffer[LINE_BUF] = {0};
    char *dynamic_buffer = NULL;
    int buffer_size = 0;
    int data_received = 0;
    time_t start_time = time(NULL);
    time_t absolute_timeout = start_time + (profile->timeout * 5); // Absolute timeout protection
    int exitcode = TIMEOUT_WAITING_NEWLINE;

#ifdef EARLY_RETURN
    int empty_read_count = 0;
    const int MAX_EMPTY_READS = 500;
#endif

    // Main reading loop
    while (difftime(time(NULL), start_time) < profile->timeout && 
           time(NULL) < absolute_timeout) {
        
        // Clear line buffer
        memset(line_buffer, 0, sizeof(line_buffer));
        
        // Attempt to read a line
        if (fgets(line_buffer, sizeof(line_buffer), fdi)) {
            data_received = 1;
            dbg_msg("Read: %s", line_buffer);

            // Reset timeout if greedy_read is enabled and we got data
            if (profile->greedy_read && strlen(line_buffer) > 0) {
                start_time = time(NULL);
            }

            // Append to message buffer if requested
            if (message != NULL) {
                int append_result = append_to_buffer(&dynamic_buffer, &buffer_size, line_buffer);
                if (append_result != SUCCESS) {
                    exitcode = append_result;
                    break;
                }
            }

            // Check for termination conditions
            int termination_result = is_at_termination_line(line_buffer, key_word);
            if (termination_result > 0) {
                if (termination_result == 2 || termination_result == 1) {
                    // Keyword match
                    dbg_msg("Keyword '%s' found", key_word ? key_word : "NULL");
                    exitcode = SUCCESS;
                } else if (key_word == NULL) {
                    // Standard termination without specific keyword requirement
                    exitcode = SUCCESS;
                } else {
                    // Standard termination but we were looking for a specific keyword
                    exitcode = KEYWORD_NOT_MATCH;
                }
                break;
            }

#ifdef EARLY_RETURN
            // Reset empty read counter on successful read
            empty_read_count = 0;
#endif
        } else {
            // Handle read failure or no data available
#ifdef EARLY_RETURN
            if (data_received) {
                empty_read_count++;
                if (empty_read_count > MAX_EMPTY_READS) {
                    dbg_msg("Early return after %d empty reads", empty_read_count);
                    exitcode = TIMEOUT_WAITING_NEWLINE;
                    break;
                }
            }
#endif
        }

        // Small delay to prevent excessive CPU usage
        usleep(5000);
    }

    // Set final exit code based on whether any data was received
    if (!data_received) {
        exitcode = COMM_ERROR;
    } else if (exitcode == TIMEOUT_WAITING_NEWLINE && time(NULL) >= absolute_timeout) {
        dbg_msg("Absolute timeout reached");
        exitcode = TIMEOUT_WAITING_NEWLINE;
    }

    // Handle message buffer
    if (message != NULL) {
        message->message = dynamic_buffer;
        message->len = buffer_size;
    } else {
        // Clean up if no message structure provided
        free(dynamic_buffer);
    }

    return exitcode;
}

int tty_write_raw(FILE *fdo, char *input)
{
    int ret;
    ret = fputs(input, fdo);
    fflush(fdo);
    usleep(100);
    if (ret < 0)
    {
        err_msg("Error writing to tty %d" , ret);
        return COMM_ERROR;
    }
    return SUCCESS;
}

int tty_write(FILE *fdo, char *input)
{
    int cmd_len, ret;
    char *cmd_line;
    cmd_len = strlen(input) + 3;
    cmd_line = (char *)malloc(cmd_len);
    if (cmd_line == NULL)
    {
        err_msg("Error allocating memory");
        return COMM_ERROR;
    }
    snprintf(cmd_line, cmd_len, "%s\r\n", input);
    ret =  tty_write_raw(fdo, cmd_line);
    free(cmd_line);
    return ret;
}
