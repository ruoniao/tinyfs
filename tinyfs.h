#define MAXLEN 8
#define MAX_FILES 32
#define MAX_BLOCKSIZE 512
#define MAX_SUBDIR_FILES 4  //one dir can include max file size
 
//define the dir node info
struct dir_entry {
    char filename[MAXLEN];
    uint8_t idx;
};
 
//just same count char as dir node size
#define FILE_BUF_SIZ  (sizeof(struct dir_entry) * MAX_SUBDIR_FILES)
 
//define the file block
struct file_blk {
    uint8_t busy;
    mode_t mode;
    uint8_t idx;
 
    union {
        uint8_t file_size;
        uint8_t dir_children;
    };
    
    //用作目录时,最大记录的条目数，不超过4个; 用作文件时，这里就是文件的buffer大小
    //one dir can exit 4 file
    union {
        struct dir_entry dir_data[MAX_SUBDIR_FILES]; 
        char file_data[FILE_BUF_SIZ];
    };
};