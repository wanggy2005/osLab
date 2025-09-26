#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

void find(char *path, char *target);

// 从路径中提取文件名
char* fmtname(char *path) {
    static char buf[DIRSIZ+1];
    char *p;
    // 查找路径中的最后一个斜杠
    for(p = path + strlen(path); p >= path && *p != '/'; p--);
    p++;
    // 返回文件名部分
    if(strlen(p) >= DIRSIZ)
        return p;
    memmove(buf, p, strlen(p));
    buf[strlen(p)] = 0;
    return buf;
}

// 递归查找函数
void find(char *path, char *target) {
    char buf[512], *p;
    int fd;
    struct dirent de;
    struct stat st;

    if((fd = open(path, 0)) < 0) {
        fprintf(2, "find: cannot open %s\n", path);
        return;
    }

    if(fstat(fd, &st) < 0) {
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        return;
    }

    // 检查当前路径是否匹配
    if(strcmp(fmtname(path), target) == 0) {
        printf("%s\n", path);
    }

    // 如果是目录则递归搜索
    if(st.type == T_DIR) {
        if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf) {
            fprintf(2, "find: path too long\n");
            close(fd);
            return;
        }
        strcpy(buf, path);
        p = buf + strlen(buf);
        *p++ = '/';
        
        while(read(fd, &de, sizeof(de)) == sizeof(de)) {
            if(de.inum == 0)
                continue;
                
            // 跳过 "." 和 ".." 目录
            if(strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
                continue;
                
            memmove(p, de.name, DIRSIZ);
            p[DIRSIZ] = 0;
            
            // 递归查找子目录
            find(buf, target);
        }
    }
    close(fd);
}

int main(int argc, char *argv[]) {
    if(argc != 3) {
        fprintf(2, "Usage: find <path> <name>\n");
        exit(1);
    }
    find(argv[1], argv[2]);
    exit(0);
}