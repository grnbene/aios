#pragma once
/* FAT32 / VFS Filesystem Subsystem for MicroSD Storage (metal/arm64/fat32.hpp).
 *
 * Implements MBR partition table parsing, FAT32 BIOS Parameter Block (BPB) decoding,
 * FAT cluster chain traversal, 8.3 / Long File Name directory parsing, and file reading.
 */
#include <cstddef>
#include <cstdint>

struct Fat32FileInfo {
  char name[64];
  uint32_t size_bytes;
  uint32_t first_cluster;
  bool is_directory;
  bool is_hidden;
};

struct Fat32FsStatus {
  bool mounted;
  char volume_label[12];
  uint32_t lba_start;
  uint32_t sectors_per_cluster;
  uint32_t bytes_per_cluster;
  uint32_t fat_start_sector;
  uint32_t data_start_sector;
  uint32_t root_cluster;
  uint32_t total_clusters;
  uint32_t free_clusters;
  uint32_t total_files_indexed;
};

/* Mount FAT32 partition from SD Card */
bool fat32_mount();

/* List files in directory (path = "/" or subpath) */
size_t fat32_list(const char* path, Fat32FileInfo* out_entries, size_t max_entries);

/* Read entire file into memory buffer */
bool fat32_read_file(const char* filename, void* dst, size_t max_len, size_t& out_len);

/* Size of a file, or false when there is no such file (or it is a directory) */
bool fat32_file_size(const char* filename, size_t& out_size);

/* Write or create file on FAT32 filesystem */
bool fat32_write_file(const char* filename, const void* src, size_t len);

/* Delete file from FAT32 filesystem */
bool fat32_delete_file(const char* filename);

/* Status and diagnostic report */
const Fat32FsStatus& fat32_status();
void fat32_report();

