# FastFinder 🚀

![image](./2a71d3ba_preview_rev_1.png)

FastFinder is a powerful command-line tool designed to quickly search for keywords in `.txt` files within a specified directory. With its multi-threading capabilities, FastFinder can process millions of lines per second, making it ideal for handling large datasets efficiently. 

## Features 🌟
- **Multi-threaded processing**: Utilizes up to 16 threads for faster execution.
- **Efficient file handling**: Reads large files with a buffer size of 1 MB.
- **User-friendly output**: Saves all matching lines to a specified output file.
- **Real-time statistics**: Provides real-time information on the number of lines read and saved.

## Usage 📚
To run FastFinder, use the following command:

```bash
fastfinder.exe <file/directory> <keyword> <output>
```

### Example
```bash
fastfinder.exe "C:\Users\user\Downloads\Telegram Desktop" wp-login output.txt
```

### Parameters
| Parameter         | Description                                       |
|-------------------|---------------------------------------------------|
| `<file/directory>`| The path to the `.txt` file or the directory to search in. |
| `<keyword>`       | The keyword to search for in the text files.     |
| `<output>`        | The name of the output file where results will be saved. |

## Installation 💻
You can download the latest release of FastFinder for Windows from the following link:

[Download FastFinder](https://github.com/lalaio1/fastfinder/releases/tag/1.0)

## Compiling FastFinder 🛠️
To compile FastFinder from source, use the following command:

```bash
x86_64-w64-mingw32-gcc -o fastfinder.exe fastfinder.c -lpthread -static -O3 -march=native -mtune=native -ffast-math -funroll-loops -flto -s
```

### Compilation Options
- `-lpthread`: Links the POSIX thread library to enable multi-threading.
- `-static`: Statically links libraries, resulting in a single executable.
- `-O3`: Enables high-level optimizations for better performance.
- `-march=native` and `-mtune=native`: Optimize the code for the current architecture.
- `-ffast-math`: Allows aggressive optimizations for floating-point math.
- `-funroll-loops`: Unrolls loops for improved performance.
- `-flto`: Enables Link Time Optimization, further optimizing the final binary.
- `-s`: Strips symbols from the output file to reduce its size.

## Code Overview 📖
FastFinder uses the following constants to manage its performance:

```c
#define MAX_THREADS 16          // Maximum number of threads for processing files.
#define BUFFER_SIZE 1048576     // Size of the buffer used for reading files (1 MB).
#define FILE_QUEUE_SIZE 1000    // Maximum number of files that can be queued for processing.
```

### Techniques Used
- **Multi-threading**: FastFinder utilizes Windows threads to process multiple files simultaneously, allowing for rapid keyword searching across large datasets.
- **Critical Sections**: To ensure thread safety when accessing shared resources, FastFinder uses critical sections to lock and unlock access to output files and the file queue.
- **Interlocked Operations**: To safely update counters for lines read and saved across threads, FastFinder employs interlocked operations that prevent race conditions.
- **Memory Management**: FastFinder dynamically allocates memory for buffers and file names, ensuring efficient use of resources.

## Performance ⚡
FastFinder is optimized to read millions of lines per second, making it one of the fastest keyword search tools available for Windows.

## Creator 👤
[GitHub: lalaio1](https://github.com/lalaio1)

## Contributing 🤝
Contributions are welcome! If you have suggestions for improvements or find bugs, please open an issue or submit a pull request.

---

Thank you for using FastFinder! Happy searching! 🎉
```
