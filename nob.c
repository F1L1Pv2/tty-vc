#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"

#include <time.h>

Cmd cmd = {0};

#ifndef WIN32
int isDirectory(const char *path) {
   struct stat statbuf;
   if (stat(path, &statbuf) != 0)
       return 0;
   return S_ISDIR(statbuf.st_mode);
}
#endif

bool traverse_directory(char* filepath, File_Paths* out){
    File_Paths children = {0};
    read_entire_dir(filepath,&children);
    for(int i = 0; i < children.count; i++){
        if(children.items[i][0] == '.') continue;
        String_Builder sb = {0};
        String_View sv = sv_from_cstr(children.items[i]);
        sv_chop_by_delim(&sv,'.');

        sb_append_cstr(&sb, filepath);
        if(sb.items[sb.count-1] != '/' && sb.items[sb.count-1] != '\\') sb_append_cstr(&sb, "/");
        sb_append_cstr(&sb,children.items[i]);
        sb_append_null(&sb);


#ifdef WIN32
        if(strlen(children.items[i]) == sv.count || sv.count == 0){
#else  
        if(isDirectory(sb.items)) {
#endif
            File_Paths inner_children = {0};
            traverse_directory(sb.items,&inner_children);
            for(int j = 0; j < inner_children.count; j++){
                da_append(out, inner_children.items[j]);
            }
            da_free(inner_children);
        }else{
            da_append(out, sb.items);
        }
    }

    da_free(children);
    return true;
};

String_View chopUntilEnd(String_View in, char ch){
    String_View sv = in;

    while(sv.count != 0 && sv.data - in.data < in.count){
        in = sv;
        sv_chop_by_delim(&sv,ch);
    }
    
    return in;
}

void filter_out_paths_ending(char** ending, size_t ending_count, File_Paths* in){
    File_Paths intermediate = {0};

    for(int i = 0; i < in->count; i++){
        String_View sv = sv_from_cstr(in->items[i]);
        if(sv.count == 0) continue;
        sv = chopUntilEnd(sv,'.');
        for(int j =0; j < ending_count; j++){
            if(strcmp(sv.data,ending[j]) == 0){
                da_append(&intermediate, in->items[i]);
                break;
            }
        }
    }

    da_free(*in);
    in->items = intermediate.items;
    in->capacity = intermediate.capacity;
    in->count = intermediate.count;
}

void filter_out_paths_dont_ending(char** ending, size_t ending_count, File_Paths* in){
    File_Paths intermediate = {0};

    for(int i = 0; i < in->count; i++){
        String_View sv = sv_from_cstr(in->items[i]);
        if(sv.count == 0) continue;
        sv = chopUntilEnd(sv,'.');
        for(int j =0; j < ending_count; j++){
            if(strcmp(sv.data,ending[j]) != 0){
                da_append(&intermediate, in->items[i]);
                break;
            }
        }
    }

    da_free(*in);
    in->items = intermediate.items;
    in->capacity = intermediate.capacity;
    in->count = intermediate.count;
}

void filter_out_paths_doesnt_contain(char* contain, File_Paths* in){
    File_Paths intermediate = {0};

    String_View prefix = sv_from_cstr(contain);
    for(int i = 0; i < in->count; i++){
        String_View sv = sv_from_cstr(in->items[i]);
        bool contain = false;
        while(sv.count > 0){
            if(sv_starts_with(sv,prefix)){
                contain = true;
                break;
            }
            sv.data++;
            sv.count--;
        }
        if(contain == false){
            da_append(&intermediate, in->items[i]);
        }
    }

    da_free(*in);
    in->items = intermediate.items;
    in->capacity = intermediate.capacity;
    in->count = intermediate.count;
}

bool build_third_party(){
    //this can be dumb build like who would modify third party
    bool result = true;

    int opuslib_exists = file_exists(
#ifdef WIN32
        "./thirdparty/opusfile.lib"
#else
        "./thirdparty/libopusfile.a"
#endif
    );
    if(opuslib_exists < 0) return false;
    if(opuslib_exists == 1) return true;
    
    File_Paths children = {0};
    File_Paths objects_children = {0};
    if(!traverse_directory("./thirdparty", &children)) return_defer(false);
    
    char *allowed[] = {"c"};
    filter_out_paths_ending(allowed, 1,&children);
    filter_out_paths_doesnt_contain("test",&children);
    filter_out_paths_doesnt_contain("arm",&children);
    filter_out_paths_doesnt_contain("dnn",&children);
    filter_out_paths_doesnt_contain("mips",&children);
    filter_out_paths_doesnt_contain("x86",&children);
    filter_out_paths_doesnt_contain("silk/fixed",&children);
    
    for(int i = 0; i < children.count; i++){
        String_Builder sb = {0};
        String_View sv = sv_from_cstr(children.items[i]);
        String_View sv2 = sv_from_cstr(children.items[i]);
        if(sv2.data[0] == '.') sv_chop_by_delim(&sv2, '.');
        sv_chop_by_delim(&sv2, '.');
        sv.count = sv2.data - sv.data;

        sb_append_buf(&sb,sv.data,sv.count);
        sb_append_cstr(&sb,"o");
        sb_append_null(&sb);
    
        cmd.count = 0;
        cmd_append(&cmd,
            "clang",
            "-ffunction-sections",
            "-fdata-sections",
            "-O3",
            "-c",
                
            "-I./thirdparty/opus/src",
            "-I./thirdparty/opus/include",
            "-I./thirdparty/opus/silk",
            "-I./thirdparty/opus/",
            "-I./thirdparty/opus/dnn",
            "-I./thirdparty/opus/celt",
            "-I./thirdparty/opus/silk/float",
            "-I./thirdparty/opusfile/include",
            "-I./thirdparty/ogg/include",
    
            children.items[i],
            "-o",
            sb.items
        );

        da_append(&objects_children, sb.items);
        if(!cmd_run_sync_and_reset(&cmd)) return_defer(false);
    }

#ifdef WIN32
    cmd_append(&cmd,"llvm-ar", "rcs", "./thirdparty/opusfile.lib");
#else
    cmd_append(&cmd,"llvm-ar", "rcs", "./thirdparty/libopusfile.a");
#endif

    for(int i = 0; i < objects_children.count; i++){
        cmd_append(&cmd, objects_children.items[i]);
    }

    if(!cmd_run_sync_and_reset(&cmd)) return_defer(false);

defer:
    da_free(children);
    da_free(objects_children);
    return result;
}

void usage(char* program){
    printf("[USAGE]: %s (client) (server)\n", program);
}

int main(int argc, char** argv){
    NOB_GO_REBUILD_URSELF(argc,argv);

    char* program = shift_args(&argc, &argv);
    
    bool build_client = true;
    bool build_server = true;

    while (argc > 0){
        char* arg = shift_args(&argc,&argv);
        if(strcmp(arg,"client") == 0){
            build_client = true;
        }

        if(strcmp(arg,"server") == 0){
            build_client = false;
        }

        if(strcmp(arg, "help") == 0){
            usage(program);
            return 0;
        }
    }

    if(!build_third_party()) return 1;

    mkdir_if_not_exists("build");

    int result = needs_rebuild1(
#ifdef _WIN32
        "build/client.exe",
#else
        "build/client",
#endif
        "src/client.cpp"
    );

    if(result < 0) return 1;

    if(build_client && result){
        cmd.count = 0;
        cmd_append(&cmd,
           "clang++",
           "-g",
           "-std=c++17",
           "src/client.cpp",
           "-o",
#ifdef _WIN32
           "build/client.exe",
#else
            "build/client",
#endif
           "-I",
           "thirdparty",
           "-I",
           "thirdparty/ogg/include",
           "-I",
           "thirdparty/opus/include",
           "-L",
           "thirdparty",
           "-lopusfile",
        );

        if(!cmd_run_sync_and_reset(&cmd)) return 1;
    }


    result = 
#ifndef _WIN32
    needs_rebuild1(
        "build/server",
        "src/server.c"
    );
#else
    1;
#endif

    if(result < 0) return 1;

    if(build_server && result){
#ifndef _WIN32
        cmd.count = 0;

        cmd_append(&cmd, "clang", "src/server.c", "-o", "build/server");
        if(!cmd_run_sync_and_reset(&cmd)) return 1;
#else
        printf("Building server on windows is not supported (who would use windows for server anyways)\n");
#endif
    }

    return 0;
}