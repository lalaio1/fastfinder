#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <windows.h>
#include <process.h>

#define MAX_THREADS 16
#define BUFFER_SIZE 1048576
#define FILE_QUEUE_SIZE 1000

typedef struct {
    char *directory;
    char *keyword;
    FILE *output;
    volatile long long lines_read;
    volatile long long lines_saved;
} ThreadArgs;

typedef struct {
    char **files;
    int front;
    int rear;
    int count;
    CRITICAL_SECTION lock;
} FileQueue;

CRITICAL_SECTION output_lock;
FileQueue file_queue;

__forceinline int contains_keyword(const char *line, const char *keyword, size_t keyword_len) {
    return strstr(line, keyword) != NULL;
}

void process_file(const char *file, const char *keyword, FILE *output, volatile long long *lines_read, volatile long long *lines_saved) {
    FILE *input = fopen(file, "rb");
    if (!input) return;

    char *buffer = (char *)malloc(BUFFER_SIZE);
    char *line_start, *line_end;
    size_t bytes_read, keyword_len = strlen(keyword);
    long long local_lines_read = 0, local_lines_saved = 0;

    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, input)) > 0) {
        line_start = buffer;
        while ((line_end = memchr(line_start, '\n', bytes_read - (line_start - buffer)))) {
            *line_end = '\0';
            local_lines_read++;

            if (contains_keyword(line_start, keyword, keyword_len)) {
                EnterCriticalSection(&output_lock);
                fprintf(output, "%s\n", line_start);
                local_lines_saved++;
                LeaveCriticalSection(&output_lock);
            }

            line_start = line_end + 1;
        }
    }

    free(buffer);
    fclose(input);

    InterlockedAdd64((LONG64 *)lines_read, local_lines_read);
    InterlockedAdd64((LONG64 *)lines_saved, local_lines_saved);
}

void enqueue_file(FileQueue *queue, const char *file) {
    EnterCriticalSection(&queue->lock);
    if (queue->count < FILE_QUEUE_SIZE) {
        queue->files[queue->rear] = _strdup(file);
        queue->rear = (queue->rear + 1) % FILE_QUEUE_SIZE;
        queue->count++;
    }
    LeaveCriticalSection(&queue->lock);
}

char *dequeue_file(FileQueue *queue) {
    char *file = NULL;
    EnterCriticalSection(&queue->lock);
    if (queue->count > 0) {
        file = queue->files[queue->front];
        queue->front = (queue->front + 1) % FILE_QUEUE_SIZE;
        queue->count--;
    }
    LeaveCriticalSection(&queue->lock);
    return file;
}

unsigned __stdcall process_files_thread(void *arg) {
    ThreadArgs *args = (ThreadArgs *)arg;
    char *file;
    while ((file = dequeue_file(&file_queue))) {
        process_file(file, args->keyword, args->output, &args->lines_read, &args->lines_saved);
        free(file);
    }
    return 0;
}

void scan_directory(const char *dir, FileQueue *queue) {
    WIN32_FIND_DATA find_data;
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*", dir);

    HANDLE find_handle = FindFirstFile(search_path, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE) return;

    do {
        if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0) continue;

        char full_path[MAX_PATH];
        snprintf(full_path, sizeof(full_path), "%s\\%s", dir, find_data.cFileName);

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_directory(full_path, queue);
        } else {
            const char *ext = strrchr(find_data.cFileName, '.');
            if (ext && _stricmp(ext, ".txt") == 0) {
                enqueue_file(queue, full_path);
            }
        }
    } while (FindNextFile(find_handle, &find_data));

    FindClose(find_handle);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <file/directory> <keyword> <output>\n", argv[0]);
        return 1;
    }

    char *path = argv[1];
    char *keyword = argv[2];
    char *output_file = argv[3];

    FILE *output = fopen(output_file, "w");
    if (!output) {
        fprintf(stderr, "Error creating output file: %s\n", output_file);
        return 1;
    }

    InitializeCriticalSection(&output_lock);
    InitializeCriticalSection(&file_queue.lock);

    file_queue.files = (char **)malloc(sizeof(char *) * FILE_QUEUE_SIZE);
    file_queue.front = file_queue.rear = file_queue.count = 0;

    struct stat path_stat;
    stat(path, &path_stat);

    if (S_ISDIR(path_stat.st_mode)) {
        scan_directory(path, &file_queue);
    } else {
        enqueue_file(&file_queue, path);
    }

    HANDLE threads[MAX_THREADS];
    ThreadArgs thread_args[MAX_THREADS];

    for (int i = 0; i < MAX_THREADS; i++) {
        thread_args[i].directory = path;
        thread_args[i].keyword = keyword;
        thread_args[i].output = output;
        thread_args[i].lines_read = 0;
        thread_args[i].lines_saved = 0;

        threads[i] = (HANDLE)_beginthreadex(NULL, 0, process_files_thread, &thread_args[i], 0, NULL);
    }

    WaitForMultipleObjects(MAX_THREADS, threads, TRUE, INFINITE);

    long long total_lines_read = 0, total_lines_saved = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        CloseHandle(threads[i]);
        total_lines_read += thread_args[i].lines_read;
        total_lines_saved += thread_args[i].lines_saved;
    }

    fclose(output);
    DeleteCriticalSection(&output_lock);
    DeleteCriticalSection(&file_queue.lock);

    for (int i = 0; i < file_queue.count; i++) {
        free(file_queue.files[i]);
    }
    free(file_queue.files);

    printf("\nSearch completed.\n");
    printf("Total lines read: %lld\n", total_lines_read);
    printf("Total lines saved: %lld\n", total_lines_saved);
    printf("Lines containing '%s' were saved to %s\n", keyword, output_file);

    return 0;
}
