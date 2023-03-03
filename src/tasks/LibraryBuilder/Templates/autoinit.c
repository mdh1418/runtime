#include <mono/metadata/assembly.h>
#include <mono/jit/jit.h>
#include <mono/jit/mono-private-unstable.h>

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

static char *bundle_path = "/data/user/0/net.dot.Android.Device_Emulator.Aot_Llvm.Test/files";

#define RUNTIMECONFIG_BIN_FILE "runtimeconfig.bin"

void register_aot_modules (void);

void
cleanup_runtime_config (MonovmRuntimeConfigArguments *args, void *user_data)
{
    free (args);
    free (user_data);
}

void register_bundled_modules ()
{
    char *file_name = RUNTIMECONFIG_BIN_FILE;
    int str_len = strlen (bundle_path) + strlen (file_name) + 1; // +1 is for the "/"
    char *file_path = (char *)malloc (sizeof (char) * (str_len +1)); // +1 is for the terminating null character
    int num_char = snprintf (file_path, (str_len + 1), "%s/%s", bundle_path, file_name);
    struct stat buffer;

    assert (num_char > 0 && num_char == str_len);

    if (stat (file_path, &buffer) == 0) {
        MonovmRuntimeConfigArguments *arg = (MonovmRuntimeConfigArguments *)malloc (sizeof (MonovmRuntimeConfigArguments));
        arg->kind = 0;
        arg->runtimeconfig.name.path = file_path;
        monovm_runtimeconfig_initialize (arg, cleanup_runtime_config, file_path);
    } else {
        free (file_path);
    }
}

static unsigned char *
load_aot_data (MonoAssembly *assembly, int size, void *user_data, void **out_handle)
{
    *out_handle = NULL;

    char path [1024];
    int res;

    MonoAssemblyName *assembly_name = mono_assembly_get_name (assembly);
    const char *aname = mono_assembly_name_get_name (assembly_name);

    res = snprintf (path, sizeof (path) - 1, "%s/%s.aotdata", bundle_path, aname);
    assert (res > 0);

    int fd = open (path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }

    void *ptr = mmap (NULL, size, PROT_READ, MAP_FILE | MAP_PRIVATE, fd, 0);
    if (ptr == MAP_FAILED) {
        close (fd);
        return NULL;
    }

    close (fd);
    *out_handle = ptr;
    return (unsigned char *) ptr;
}

static void
free_aot_data (MonoAssembly *assembly, int size, void *user_data, void *handle)
{
    munmap (handle, size);
}

void runtime_init_callback ()
{
    chdir (bundle_path);
    
    // register all aot modules
    register_aot_modules ();

    // register all bundled modules
    register_bundled_modules ();

    // mono_set_assemblies_path
    char *assemblyPath = bundle_path;
    mono_set_assemblies_path ((assemblyPath && assemblyPath[0] != '\0') ? assemblyPath : ".\\");

// #if JIT_DISABLED
    mono_jit_set_aot_only (true); // Would `mono_jit_set_aot_mode (MONO_AOT_MODE_FULL);` be better here
// #endif

#if DEBUG_ENABLED
    // bool wait_for_debugger = false;
    mono_debug_init (MONO_DEBUG_FORMAT_MONO);
    if (wait_for_debugger) {
        char* options[] = { "--debugger-agent=transport=dt_socket,server=y,address=0.0.0.0:55555" };
        mono_jit_parse_options (1, options);
    }
#endif

    // mono_install_assembly_preload_hook (mono_droid_assembly_preload_hook, NULL);
    mono_install_load_aot_data_hook (load_aot_data, free_aot_data, NULL);

    // mono_jit_init_version
    mono_jit_init ("dotnet.android");

    // Load assemblies with UnmanagedCallersOnly exported methods
    mono_assembly_open("Android.Device_Emulator.Aot_Llvm.Test.dll", NULL);
}

void init_mono_runtime ()
{
    mono_set_runtime_init_callback (&runtime_init_callback);
}

void __attribute__((constructor))
autoinit ()
{
    init_mono_runtime ();
}
