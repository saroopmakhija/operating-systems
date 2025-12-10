/*
 *  Copyright (C) 2025 CS416 Rutgers CS
 *	Rutgers Tiny File System
 *	File:	rufs.c
 *
 */

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <libgen.h>
#include <limits.h>
#include <time.h>

#include "block.h"
#include "rufs.h"

char diskfile_path[PATH_MAX];

// Declare your in-memory data structures here

struct superblock superblockmain;
/* 
 * Get available inode number from bitmap
 */
int get_avail_ino() {

	// Step 1: Read inode bitmap from disk
	unsigned char buf[BLOCK_SIZE]; 
	bitmap_t inode_bitmap;
    int ret;

	ret = bio_read(superblockmain.i_bitmap_blk, buf);
    if (ret < 0) {
		perror("get_avail_ino: bio_read inode bitmap failed");
        return -1;
    }
	inode_bitmap = (bitmap_t)buf;
	// Step 2: Traverse inode bitmap to find an available slot
	for (int i = 0; i < superblockmain.max_inum; i++) {
        if (get_bitmap(inode_bitmap, i) == 0) {
            set_bitmap(inode_bitmap, i);

			ret = bio_write(superblockmain.i_bitmap_blk, buf);
			
			if(ret < 0){
				perror("get_avail_ino: bio_write inode bitmap failed");
				return -1;
			}
			return i;
		}
	}
	return -1;
}

/* 
 * Get available data block number from bitmap
 */
int get_avail_blkno() {

	// Step 1: Read data block bitmap from disk
	unsigned char buf[BLOCK_SIZE]; 
	bitmap_t datanode_bitmap;
	int ret;

	ret = bio_read(superblockmain.d_bitmap_blk, buf);
    if (ret < 0) {
		perror("get_avail_dno: bio_read datanode bitmap failed");
        return -1;
    }
	datanode_bitmap = (bitmap_t)buf;
	// Step 2: Traverse data block bitmap to find an available slot
	for (int i = 0; i < superblockmain.max_dnum; i++) {
        if (get_bitmap(datanode_bitmap, i) == 0) {
            set_bitmap(datanode_bitmap, i);

			ret = bio_write(superblockmain.d_bitmap_blk, buf);
			
			if(ret < 0){
				perror("get_avail_dno: bio_write datanode bitmap failed");
				return -1;
			}
			return superblockmain.d_start_blk + i;		
		}
	}

	return -1;
}

/* 
 * inode operations
 */
int readi(uint16_t ino, struct inode *inode) {

	// Step 1: Get the inode's on-disk block number
	// Step 2: Get offset of the inode in the inode on-disk block
	// Step 3: Read the block from disk and then copy into inode structure
	if (ino >= superblockmain.max_inum) {return -1;}

	int inodes_per_block = BLOCK_SIZE / sizeof(struct inode);
    int blk_index = ino / inodes_per_block;
    int offset_in_block = ino % inodes_per_block;

	int disk_block_num = superblockmain.i_start_blk + blk_index;

	unsigned char buf[BLOCK_SIZE];
    int ret = bio_read(disk_block_num, buf);
    if (ret < 0) {
        perror("readi: bio_read failed");
        return -1;
    }
    struct inode *inode_block = (struct inode *)buf;
    memcpy(inode, &inode_block[offset_in_block], sizeof(struct inode));
	return 0;
}

int writei(uint16_t ino, struct inode *inode) {

	// Step 1: Get the block number where this inode resides on disk
	// Step 2: Get the offset in the block where this inode resides on disk
	// Step 3: Write inode to disk 

	if (ino >= superblockmain.max_inum) {
        return -1;
    }

	int inodes_per_block = BLOCK_SIZE / sizeof(struct inode);
    int blk_index = ino / inodes_per_block;
    int offset_in_block = ino % inodes_per_block;

	int disk_block_num = superblockmain.i_start_blk + blk_index;

	unsigned char buf[BLOCK_SIZE];
    int ret = bio_read(disk_block_num, buf);
    if (ret < 0) {
        perror("writei: bio_read failed");
        return -1;
    }
    struct inode *inode_block = (struct inode *)buf;
	memcpy(&inode_block[offset_in_block], inode, sizeof(struct inode));
	
	ret = bio_write(disk_block_num, buf);
    if (ret < 0) {
        perror("writei: bio_write failed");
        return -1;
    }

	return 0;
}


/* 
 * directory operations
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {

	// Step 1: Call readi() to get the inode using ino (inode number of current directory)
	// Step 2: Get data block of current directory from inode
	// Step 3: Read directory's data block and check each directory entry.
	// If the name matches, then copy directory entry to dirent structure
	struct inode dir_inode; 
	int ret = readi(ino, &dir_inode);
	if(ret <  0){
		perror("readi failed in dir_find");
		return -1;
	}
    int entries_per_block = BLOCK_SIZE / sizeof(struct dirent);
    unsigned char buf[BLOCK_SIZE];
	for (int i = 0; i < 16; i++) {
        int blkno = dir_inode.direct_ptr[i];
        // Skip unused pointers
        if (blkno == 0) {
            continue;
        }

        ret = bio_read(blkno, buf);
        if (ret < 0) {
            perror("dir_find: bio_read failed");
            return -1;
        }
		struct dirent *entries = (struct dirent *)buf;

		for (int j = 0; j < entries_per_block; j++) {
            if (!entries[j].valid) {continue;}
			if (entries[j].len == name_len && strncmp(entries[j].name, fname, name_len) == 0) {
                // Found the entry, copy it out
                *dirent = entries[j];
                return 0;
				}
	}
	}

	return -1;
}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {

	// Step 1: Read dir_inode's data block and check each directory entry of dir_inode
	// Step 2: Check if fname (directory name) is already used in other entries
	// Step 3: Add directory entry in dir_inode's data block and write to disk
	// Allocate a new data block for this directory if it does not exist
	// Update directory inode
	// Write directory entry

	int ret;
	unsigned char buf[BLOCK_SIZE];
	int entries_per_block = BLOCK_SIZE / sizeof(struct dirent);

	if (name_len == 0 || name_len >= 208) {
        return -1;
    }

	int free_blk_index = -1;
	int free_entry_index = -1;

	//step 1 and 2

	for(int i =0; i < 16; i++){
		int blkno = dir_inode.direct_ptr[i];

		if(blkno == 0){
			if(free_blk_index == -1){
				free_blk_index = i;
			}
			continue;
		}

		ret = bio_read(blkno, buf);
		if (ret < 0) {
            perror("dir_add: bio_read failed");
            return -1;
        }
		struct dirent *entries = (struct dirent *)buf;

		for(int j = 0; j < entries_per_block; j++){
			if(entries[j].valid){
				if (entries[j].len == (uint16_t)name_len &&
                    strncmp(entries[j].name, fname, name_len) == 0) {
                    return -1;
                }
			}
			else{
				if(free_entry_index == -1){
					free_blk_index = i;
					free_entry_index = j;
				}
			}
		}
	}

	if(free_entry_index >= 0){
		int blkno = dir_inode.direct_ptr[free_blk_index];

		ret = bio_read(blkno, buf);
		if (ret < 0) {
            perror("dir_add: bio_read (reuse slot) failed");
            return -1;
        }
		struct dirent *entries = (struct dirent *)buf;
        struct dirent *de = &entries[free_entry_index];

		de->ino = f_ino;
		de->valid = 1;
        memset(de->name, 0, sizeof(de->name));
        memcpy(de->name, fname, name_len);
        de->len = (uint16_t)name_len;

		ret = bio_write(blkno, buf);
        if (ret < 0) {
            perror("dir_add: bio_write (reuse slot) failed");
            return -1;
        }
	}else {
        /* Case 2: no free slot in existing blocks, need a new data block */
        if (free_blk_index < 0) {
            /* No place to attach a new block */
            return -1;
        }

		int blkno = get_avail_blkno();
        if (blkno < 0) {
            return -1;
        }

		dir_inode.direct_ptr[free_blk_index] = blkno;

		memset(buf, 0, BLOCK_SIZE);
        struct dirent *entries = (struct dirent *)buf;
		entries[0].ino = f_ino;
        entries[0].valid = 1;
        memset(entries[0].name, 0, sizeof(entries[0].name));
        memcpy(entries[0].name, fname, name_len);
        entries[0].len = (uint16_t)name_len;

		ret = bio_write(blkno, buf);
		if (ret < 0) {
            perror("dir_add: bio_write (new block) failed");
            return -1;
        }
	}
	dir_inode.size += sizeof(struct dirent);
    dir_inode.vstat.st_size = dir_inode.size;
    time(&dir_inode.vstat.st_mtime);

    // Persist updated directory inode 
    ret = writei(dir_inode.ino, &dir_inode);
    if (ret < 0) {
        perror("dir_add: writei failed");
        return -1;
    }
	return 0;
}


/* 
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
	
	// Step 1: Resolve the path name, walk through path, and finally, find its inode.
	// Note: You could either implement it in a iterative way or recursive way
	if (path == NULL || inode == NULL) {
        return -1;
    }
	if (strcmp(path, "/") == 0 || strcmp(path, "") == 0) {
        if (readi(ino, inode) < 0) {
            return -1;
        }
        return 0;
    }
	char local_path[PATH_MAX];
    struct dirent dentry;
    uint16_t curr_ino = ino;

	strncpy(local_path, path, PATH_MAX);
    local_path[PATH_MAX - 1] = '\0';
    
	char *p = local_path;
    if (*p == '/') {
        p++;
    }

	char *token = strtok(p, "/");

	while (token != NULL) {
        size_t name_len = strlen(token);

        // Look up this component in the current directory 
        if (dir_find(curr_ino, token, name_len, &dentry) != 0) {
            // Component not found 
            return -1;
        }

        // Move to the child inode 
        curr_ino = dentry.ino;
        token = strtok(NULL, "/");
    }

	if (readi(curr_ino, inode) < 0) {
        return -1;
    }

	return 0;
}

/* 
 * Make file system
 */
 int rufs_mkfs() {

    // 1. Create/initialize the disk file
    dev_init(diskfile_path);

    unsigned char buf[BLOCK_SIZE];
    int inodes_per_block = BLOCK_SIZE / sizeof(struct inode);
    int inode_blocks = (MAX_INUM + inodes_per_block - 1) / inodes_per_block;

    // 2. Initialize in-memory superblock
    memset(&superblockmain, 0, sizeof(struct superblock));
    superblockmain.magic_num    = MAGIC_NUM;
    superblockmain.max_inum     = MAX_INUM;
    superblockmain.max_dnum     = MAX_DNUM;
    superblockmain.i_bitmap_blk = 1;
    superblockmain.d_bitmap_blk = 2;
    superblockmain.i_start_blk  = 3;
    superblockmain.d_start_blk  = 3 + inode_blocks;

    // 3. Write superblock to disk (block 0)
    memset(buf, 0, BLOCK_SIZE);
    memcpy(buf, &superblockmain, sizeof(struct superblock));
    if (bio_write(0, buf) < 0) {
        perror("rufs_mkfs: bio_write superblock failed");
        return -1;
    }

    // 4. Initialize inode bitmap (block 1), mark inode 0 as used (root)
    memset(buf, 0, BLOCK_SIZE);
    set_bitmap(buf, 0);
    if (bio_write(superblockmain.i_bitmap_blk, buf) < 0) {
        perror("rufs_mkfs: bio_write inode bitmap failed");
        return -1;
    }

    // 5. Initialize data bitmap (block 2), mark data block index 0 as used (root dir block)
    memset(buf, 0, BLOCK_SIZE);
    set_bitmap(buf, 0);
    if (bio_write(superblockmain.d_bitmap_blk, buf) < 0) {
        perror("rufs_mkfs: bio_write data bitmap failed");
        return -1;
    }

    // 6. Zero out inode region
    memset(buf, 0, BLOCK_SIZE);
    for (int i = 0; i < inode_blocks; i++) {
        if (bio_write(superblockmain.i_start_blk + i, buf) < 0) {
            perror("rufs_mkfs: bio_write inode region failed");
            return -1;
        }
    }

    // 7. Initialize root inode (inode 0)
    struct inode root;
    memset(&root, 0, sizeof(struct inode));

    root.ino   = 0;
    root.valid = 1;
    root.type  = S_IFDIR;
    root.link  = 2;                    // "." and ".."

    root.vstat.st_mode  = S_IFDIR | 0755;
    root.vstat.st_nlink = 2;
    root.vstat.st_uid   = getuid();
    root.vstat.st_gid   = getgid();
    root.vstat.st_size  = 0;
    time(&root.vstat.st_mtime);

    int root_data_blkno = superblockmain.d_start_blk;
    root.direct_ptr[0]  = root_data_blkno;

    // 8. Build root directory block with "." and ".."
    memset(buf, 0, BLOCK_SIZE);
    struct dirent *entries = (struct dirent *)buf;

    // "."
    entries[0].ino   = 0;
    entries[0].valid = 1;
    memset(entries[0].name, 0, sizeof(entries[0].name));
    entries[0].name[0] = '.';
    entries[0].len   = 1;

    // ".."
    entries[1].ino   = 0;
    entries[1].valid = 1;
    memset(entries[1].name, 0, sizeof(entries[1].name));
    entries[1].name[0] = '.';
    entries[1].name[1] = '.';
    entries[1].len   = 2;

    if (bio_write(root_data_blkno, buf) < 0) {
        perror("rufs_mkfs: bio_write root dir block failed");
        return -1;
    }

    root.size = 2 * sizeof(struct dirent);
    root.vstat.st_size = root.size;
    time(&root.vstat.st_mtime);

    // 9. Persist root inode
    if (writei(0, &root) < 0) {
        perror("rufs_mkfs: writei root inode failed");
        return -1;
    }

    return 0;
}




/* 
 * FUSE file operations
 */
 static void *rufs_init(struct fuse_conn_info *conn) {

    unsigned char buf[BLOCK_SIZE];
    int ret;

    // Try to open existing disk file
    ret = dev_open(diskfile_path);
    if (ret < 0) {
        // No disk file, create and format
        if (rufs_mkfs() < 0) {
            return NULL;
        }
    } else {
        // Disk file exists, check superblock magic
        ret = bio_read(0, buf);
        if (ret < 0) {
            perror("rufs_init: bio_read superblock failed");
            return NULL;
        }
        struct superblock *sb = (struct superblock *)buf;
        if (sb->magic_num != MAGIC_NUM) {
            // Not our filesystem, reformat
            if (rufs_mkfs() < 0) {
                return NULL;
            }
        } else {
            // Valid filesystem, load superblock into memory
            superblockmain = *sb;
            (void)conn;
            return NULL;
        }
    }

    // After mkfs, load superblock again
    ret = bio_read(0, buf);
    if (ret < 0) {
        perror("rufs_init: bio_read superblock after mkfs failed");
        return NULL;
    }
    superblockmain = *(struct superblock *)buf;

    (void)conn;
    return NULL;
}

static void rufs_destroy(void *userdata) {

	// Step 1: De-allocate in-memory data structures
	//saroop check

	// Step 2: Close diskfile
	dev_close();

}

static int rufs_getattr(const char *path, struct stat *stbuf) {

	// Step 1: call get_node_by_path() to get inode from path
	struct inode inode;
	int ret;
	ret = get_node_by_path(path,0, &inode);
	if(ret != 0){
		return -ENOENT;
	}
	// Step 2: fill attribute of file into stbuf from inode
	memset(stbuf, 0, sizeof(struct stat));
	
	*stbuf = inode.vstat;

	// stbuf->st_mode   = S_IFDIR | 0755;
	// stbuf->st_nlink  = 2;
	// time(&stbuf->st_mtime);

	return 0;
}

static int rufs_opendir(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path
	// Step 2: If not find, return -1

	struct inode inode;
	int ret = get_node_by_path(path, 0, &inode);
	if(ret != 0){
		return -ENOENT;
	}
	if(!S_ISDIR(inode.vstat.st_mode)){
		return -ENOTDIR;
	}

	return 0;
}

static int rufs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path
	// Step 2: Read directory entries from its data blocks, and copy them to filler
	
	struct inode dir_inode;
	int ret = get_node_by_path(path, 0, &dir_inode);
	if(ret != 0){
		return -ENOENT;
	}
	if (!S_ISDIR(dir_inode.vstat.st_mode)) {
        return -ENOTDIR;
	}
	

	unsigned char buf[BLOCK_SIZE];
	int entries_per_block = BLOCK_SIZE / sizeof(struct dirent);

	for (int i = 0; i < 16; i++) {
        int blkno = dir_inode.direct_ptr[i];
        if (blkno == 0) {
            continue;
        }

        ret = bio_read(blkno, buf);
        if (ret < 0) {
            perror("rufs_readdir: bio_read failed");
            return -EIO;
        }

        struct dirent *entries = (struct dirent *)buf;

        for (int j = 0; j < entries_per_block; j++) {
            if (!entries[j].valid) {
                continue;
            }

            filler(buffer, entries[j].name, NULL, 0);
        }
    }

	return 0;
}


static int rufs_mkdir(const char *path, mode_t mode) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name
	// Step 2: Call get_node_by_path() to get inode of parent directory
	// Step 3: Call get_avail_ino() to get an available inode number
	// Step 4: Call dir_add() to add directory entry of target directory to parent directory
	// Step 5: Update inode for target directory
	// Step 6: Call writei() to write inode to disk

	if (strcmp(path, "/") == 0 || strcmp(path, "") == 0) {
        return -EEXIST;
    }
	//splitting path into parent path and dir name
	char path_copy1[PATH_MAX];
    char path_copy2[PATH_MAX];

	strncpy(path_copy1, path, PATH_MAX);
    path_copy1[PATH_MAX - 1] = '\0';
    strncpy(path_copy2, path, PATH_MAX);
    path_copy2[PATH_MAX - 1] = '\0';

	char *parent_path = dirname(path_copy1);
    char *new_name    = basename(path_copy2);

	size_t name_len = strlen(new_name);
    if (name_len == 0 || name_len >= sizeof(((struct dirent *)0)->name)) {
        return -EINVAL;
    }

	struct inode parent_inode;
    int ret = get_node_by_path(parent_path, 0, &parent_inode);
    if (ret != 0) {
        return -ENOENT;
    }

	if (!S_ISDIR(parent_inode.vstat.st_mode)) {
        return -ENOTDIR;
    }

    //  check if name already exists under parent
    struct dirent existing;
    if (dir_find(parent_inode.ino, new_name, name_len, &existing) == 0) {
        // entry with this name already exists
        return -EEXIST;
    }

	  // allocate new inode number
	  int new_ino = get_avail_ino();
	  if (new_ino < 0) {
		  return -ENOSPC;
	  }
	  int new_dir_blkno = get_avail_blkno();
	  if (new_dir_blkno < 0) {
		  return -ENOSPC;
	  }
  
	  struct inode new_dir;
	  memset(&new_dir, 0, sizeof(struct inode));
  
	  new_dir.ino = (uint16_t)new_ino;
	  new_dir.valid = 1;
	  new_dir.type = S_IFDIR;
	  new_dir.link = 2;  // "." and ".."
	  new_dir.direct_ptr[0] = new_dir_blkno;
  
	  new_dir.size = 0;
	  new_dir.vstat.st_mode = S_IFDIR | (mode & 0777);
	  new_dir.vstat.st_nlink = 2;
	  new_dir.vstat.st_uid = getuid();
	  new_dir.vstat.st_gid = getgid();
	  new_dir.vstat.st_size = 0;
	  time(&new_dir.vstat.st_mtime);

	  unsigned char buf[BLOCK_SIZE];
	  memset(buf, 0, BLOCK_SIZE);
	  struct dirent *entries = (struct dirent *)buf;

	    // "."
		entries[0].ino = new_dir.ino;
		entries[0].valid = 1;
		memset(entries[0].name, 0, sizeof(entries[0].name));
		entries[0].name[0] = '.';
		entries[0].len = 1;
	
		// ".."
		entries[1].ino = parent_inode.ino;
		entries[1].valid = 1;
		memset(entries[1].name, 0, sizeof(entries[1].name));
		entries[1].name[0] = '.';
		entries[1].name[1] = '.';
		entries[1].len = 2;

		new_dir.size = 2 * sizeof(struct dirent);
		new_dir.vstat.st_size = new_dir.size;
		time(&new_dir.vstat.st_mtime);
		ret = bio_write(new_dir_blkno, buf);
		if (ret < 0) {
    		perror("rufs_mkdir: bio_write new dir block failed");
    		return -EIO;	
		}


		ret = dir_add(parent_inode, new_dir.ino, new_name, name_len);
		if (ret < 0) {
			return ret;
		}	

	
	    ret = writei(new_dir.ino, &new_dir);
    if (ret < 0) {
        perror("rufs_mkdir: writei new dir failed");
        return -EIO;
    }

    return 0;

}

static int rufs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name
	// Step 2: Call get_node_by_path() to get inode of parent directory
	// Step 3: Call get_avail_ino() to get an available inode number
	// Step 4: Call dir_add() to add directory entry of target file to parent directory
	// Step 5: Update inode for target file
	// Step 6: Call writei() to write inode to disk

	if (strcmp(path, "/") == 0 || strcmp(path, "") == 0) {
        return -EEXIST;
    }

    // split path into parent directory and file name
    char path_copy1[PATH_MAX];
    char path_copy2[PATH_MAX];

    strncpy(path_copy1, path, PATH_MAX);
    path_copy1[PATH_MAX - 1] = '\0';
    strncpy(path_copy2, path, PATH_MAX);
    path_copy2[PATH_MAX - 1] = '\0';

    char *parent_path = dirname(path_copy1);
    char *file_name   = basename(path_copy2);

    size_t name_len = strlen(file_name);
    if (name_len == 0 || name_len >= sizeof(((struct dirent *)0)->name)) {
        return -EINVAL;
    }

    // get parent directory inode
    struct inode parent_inode;
    int ret = get_node_by_path(parent_path, 0, &parent_inode);
    if (ret != 0) {
        return -ENOENT;
    }

    if (!S_ISDIR(parent_inode.vstat.st_mode)) {
        return -ENOTDIR;
    }

    // check if an entry with this name already exists
    struct dirent existing;
    if (dir_find(parent_inode.ino, file_name, name_len, &existing) == 0) {
        // Name already exists in this directory
        return -EEXIST;
    }

    // allocate a new inode number
    int new_ino = get_avail_ino();
    if (new_ino < 0) {
        return -ENOSPC;
    }

    // initialize the new file inode
    struct inode new_file;
    memset(&new_file, 0, sizeof(struct inode));

    new_file.ino   = (uint16_t)new_ino;
    new_file.valid = 1;
    new_file.type  = S_IFREG;   
    new_file.link  = 1;
    new_file.size  = 0;

    // vstat fields
    new_file.vstat.st_mode  = S_IFREG | (mode & 0777);
    new_file.vstat.st_nlink = 1;
    new_file.vstat.st_uid   = getuid();
    new_file.vstat.st_gid   = getgid();
    new_file.vstat.st_size  = 0;
    time(&new_file.vstat.st_mtime);

    // add directory entry for this file into parent directory
    ret = dir_add(parent_inode, new_file.ino, file_name, name_len);
    if (ret < 0) {
        return ret;
    }

    // write the new file inode to disk
    ret = writei(new_file.ino, &new_file);
    if (ret < 0) {
        perror("rufs_create: writei new file failed");
        return -EIO;
    }
    (void)fi;

	return 0;
}

static int rufs_open(const char *path, struct fuse_file_info *fi) {
	// Step 1: Call get_node_by_path() to get inode from path
	// Step 2: If not find, return -1

	struct inode inode;
    int ret = get_node_by_path(path, 0, &inode);

    if (ret != 0) {
        return -ENOENT;   // file does not exist
    }
	if (S_ISDIR(inode.vstat.st_mode)) {
        return -EISDIR;
    }
	(void)fi;

	return 0;
}

static int rufs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {

	// Step 1: You could call get_node_by_path() to get inode from path
	// Step 2: Based on size and offset, read its data blocks from disk
	// Step 3: copy the correct amount of data from offset to buffer
	// Note: this function should return the amount of bytes you copied to buffer
	struct inode inode;
    int ret;

	ret = get_node_by_path(path, 0, &inode);
    if (ret != 0) {
        return -ENOENT;
    }

	if (S_ISDIR(inode.vstat.st_mode)) {
        return -EISDIR;
    }

    // If offset is beyond file size, nothing to read
    if ((size_t)offset >= inode.size) {
        return 0;
    }
    size_t max_can_read = inode.size - offset;
    size_t to_read = size < max_can_read ? size : max_can_read;

    unsigned char blkbuf[BLOCK_SIZE];
    size_t copied = 0;

	// Step 3: read data block by block
	while (copied < to_read) {
		size_t cur_off = offset + copied;
		int    blk_index = cur_off / BLOCK_SIZE;
		size_t blk_off = cur_off % BLOCK_SIZE;

		if (blk_index >= 16) {
            break;
        }
		int blkno = inode.direct_ptr[blk_index];

		size_t left_total = to_read - copied;
		size_t left_in_block = BLOCK_SIZE - blk_off;
		size_t chunk = left_total < left_in_block ? left_total : left_in_block;

		if (blkno == 0) {
            // unallocated block, treat as zero
            memset(buffer + copied, 0, chunk);
            copied += chunk;
            continue;
        }
		ret = bio_read(blkno, blkbuf);
        if (ret < 0) {
            perror("rufs_read: bio_read failed");
            return -EIO;
        }

        memcpy(buffer + copied, blkbuf + blk_off, chunk);
        copied += chunk;

	}


	return (int)copied;
}

static int rufs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {

	struct inode inode;
	int ret;

	// Step 1: get inode from path
	ret = get_node_by_path(path, 0, &inode);
	if (ret != 0) {
	return -ENOENT;
	}

	if (S_ISDIR(inode.vstat.st_mode)) {
	return -EISDIR;
	}

	if (size == 0) {
	return 0;
	}

	unsigned char blkbuf[BLOCK_SIZE];
	size_t written = 0;

	// Step 2 and 3: write data block by block
	while (written < size) {
	size_t cur_off   = offset + written;
	int    blk_index = cur_off / BLOCK_SIZE;
	size_t blk_off   = cur_off % BLOCK_SIZE;

	// Only support first 16 direct blocks
	if (blk_index >= 16) {
	break;
	}

	int blkno = inode.direct_ptr[blk_index];

	if (blkno == 0) {
	// Allocate a new data block
	blkno = get_avail_blkno();
	if (blkno < 0) {
	break;  // out of space
	}
	inode.direct_ptr[blk_index] = blkno;
	memset(blkbuf, 0, BLOCK_SIZE);
	} else {
	// Read existing block
	ret = bio_read(blkno, blkbuf);
	if (ret < 0) {
	perror("rufs_write: bio_read failed");
	return -EIO;
	}
	}

	size_t left_total    = size - written;
	size_t left_in_block = BLOCK_SIZE - blk_off;
	size_t chunk         = left_total < left_in_block ? left_total : left_in_block;

	memcpy(blkbuf + blk_off, buffer + written, chunk);

	ret = bio_write(blkno, blkbuf);
	if (ret < 0) {
	perror("rufs_write: bio_write failed");
	return -EIO;
	}

	written += chunk;
	}

	// Step 4: update inode size and timestamps
	size_t new_end = offset + written;
	if (new_end > inode.size) {
	inode.size = new_end;
	inode.vstat.st_size = inode.size;
	}
	time(&inode.vstat.st_mtime);

	ret = writei(inode.ino, &inode);
	if (ret < 0) {
	perror("rufs_write: writei failed");
	return -EIO;
	}

	(void)fi;
	return (int)written;
}



/* 
 * Functions you DO NOT need to implement for this project
 * (stubs provided for completeness)
 */

static int rufs_rmdir(const char *path) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_releasedir(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_unlink(const char *path) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_truncate(const char *path, off_t size) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_release(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_flush(const char * path, struct fuse_file_info * fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_utimens(const char *path, const struct timespec tv[2]) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}


static struct fuse_operations rufs_ope = {
	.init		= rufs_init,
	.destroy	= rufs_destroy,

	.getattr	= rufs_getattr,
	.readdir	= rufs_readdir,
	.opendir	= rufs_opendir,
	.mkdir		= rufs_mkdir,

	.create		= rufs_create,
	.open		= rufs_open,
	.read 		= rufs_read,
	.write		= rufs_write,

	//Operations that you don't have to implement.
	.rmdir		= rufs_rmdir,
	.releasedir	= rufs_releasedir,
	.unlink		= rufs_unlink,
	.truncate   = rufs_truncate,
	.flush      = rufs_flush,
	.utimens    = rufs_utimens,
	.release	= rufs_release
};


int main(int argc, char *argv[]) {
	int fuse_stat;

	getcwd(diskfile_path, PATH_MAX);
	strcat(diskfile_path, "/DISKFILE");

	fuse_stat = fuse_main(argc, argv, &rufs_ope, NULL);

	return fuse_stat;
}

