#include <mono/utils/mono-publib.h>
#include <mono/utils/mono-logger.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/class.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/mono-gc.h>
#include <mono/metadata/object.h>
#include <mono/metadata/exception.h>
#include <mono/jit/jit.h>
#include <mono/jit/mono-private-unstable.h>

#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <jni.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <sys/mman.h>
#include <assert.h>
#include <unistd.h>

/*
    Note that this will expand into auto-initialization the runtime, but for now
    it makes sure to keep the monovm_initialize and monovm_runtimeconfig_initialize symbols
*/

static char *bundle_path = "/data/user/0/net.dot.Android.Device_Emulator.Aot_Llvm.Test/files";

#define LOG_INFO(fmt, ...) __android_log_print(ANDROID_LOG_DEBUG, "DOTNET", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, "DOTNET", fmt, ##__VA_ARGS__)

#if defined(__arm__)
#define ANDROID_RUNTIME_IDENTIFIER "android-arm"
#elif defined(__aarch64__)
#define ANDROID_RUNTIME_IDENTIFIER "android-arm64"
#elif defined(__i386__)
#define ANDROID_RUNTIME_IDENTIFIER "android-x86"
#elif defined(__x86_64__)
#define ANDROID_RUNTIME_IDENTIFIER "android-x64"
#else
#error Unknown architecture
#endif

#define RUNTIMECONFIG_BIN_FILE "runtimeconfig.bin"

void register_aot_modules (void);

void keep_init()
{
    const char* appctx_keys[3];
    appctx_keys[0] = "RUNTIME_IDENTIFIER";
    appctx_keys[1] = "APP_CONTEXT_BASE_DIRECTORY";
    appctx_keys[2] = "System.TimeZoneInfo.LocalDateTimeOffset";

	const char* appctx_values[3];
    appctx_values[0] = ANDROID_RUNTIME_IDENTIFIER;
    appctx_values[1] = bundle_path;
    appctx_values[2] = "0";

    int ret = monovm_initialize (3, appctx_keys, appctx_values);
}

void
cleanup_runtime_config (MonovmRuntimeConfigArguments *args, void *user_data)
{
    free (args);
    free (user_data);
}

static unsigned char *
load_aot_data (MonoAssembly *assembly, int size, void *user_data, void **out_handle)
{
    *out_handle = NULL;

    char path [1024];
    int res;

    MonoAssemblyName *assembly_name = mono_assembly_get_name (assembly);
    const char *aname = mono_assembly_name_get_name (assembly_name);

    LOG_INFO ("Looking for aot data for assembly '%s'.", aname);
    res = snprintf (path, sizeof (path) - 1, "%s/%s.aotdata", bundle_path, aname);
    assert (res > 0);

    int fd = open (path, O_RDONLY);
    if (fd < 0) {
        LOG_INFO ("Could not load the aot data for %s from %s: %s\n", aname, path, strerror (errno));
        return NULL;
    }

    void *ptr = mmap (NULL, size, PROT_READ, MAP_FILE | MAP_PRIVATE, fd, 0);
    if (ptr == MAP_FAILED) {
        LOG_INFO ("Could not map the aot file for %s: %s\n", aname, strerror (errno));
        close (fd);
        return NULL;
    }

    close (fd);
    LOG_INFO ("Loaded aot data for %s.\n", aname);
    *out_handle = ptr;
    return (unsigned char *) ptr;
}

static void
free_aot_data (MonoAssembly *assembly, int size, void *user_data, void *handle)
{
    munmap (handle, size);
}

static MonoAssembly*
mono_droid_load_assembly (const char *name, const char *culture)
{
    char filename [1024];
    char path [1024];
    int res;

    LOG_INFO ("assembly_preload_hook: %s %s %s\n", name, culture, bundle_path);

    int len = strlen (name);
    int has_extension = len > 3 && name [len - 4] == '.' && (!strcmp ("exe", name + (len - 3)) || !strcmp ("dll", name + (len - 3)));

    // add extensions if required.
    strlcpy (filename, name, sizeof (filename));
    if (!has_extension) {
        strlcat (filename, ".dll", sizeof (filename));
    }

    if (culture && strcmp (culture, ""))
        res = snprintf (path, sizeof (path) - 1, "%s/%s/%s", bundle_path, culture, filename);
    else
        res = snprintf (path, sizeof (path) - 1, "%s/%s", bundle_path, filename);
    assert (res > 0);

    struct stat buffer;
    if (stat (path, &buffer) == 0) {
        MonoAssembly *assembly = mono_assembly_open (path, NULL);
        assert (assembly);
        return assembly;
    }
    return NULL;
}

static MonoAssembly*
mono_droid_assembly_preload_hook (MonoAssemblyName *aname, char **assemblies_path, void* user_data)
{
    const char *name = mono_assembly_name_get_name (aname);
    const char *culture = mono_assembly_name_get_culture (aname);
    return mono_droid_load_assembly (name, culture);
}

char *
strdup_printf (const char *msg, ...)
{
    va_list args;
    char *formatted = NULL;
    va_start (args, msg);
    vasprintf (&formatted, msg, args);
    va_end (args);
    return formatted;
}

static MonoObject *
mono_droid_fetch_exception_property (MonoObject *obj, const char *name, bool is_virtual)
{
    MonoMethod *get = NULL;
    MonoMethod *get_virt = NULL;
    MonoObject *exc = NULL;

    get = mono_class_get_method_from_name (mono_get_exception_class (), name, 0);
    if (get) {
        if (is_virtual) {
            get_virt = mono_object_get_virtual_method (obj, get);
            if (get_virt)
                get = get_virt;
        }

        return (MonoObject *) mono_runtime_invoke (get, obj, NULL, &exc);
    } else {
        printf ("Could not find the property System.Exception.%s", name);
    }

    return NULL;
}

static char *
mono_droid_fetch_exception_property_string (MonoObject *obj, const char *name, bool is_virtual)
{
    MonoString *str = (MonoString *) mono_droid_fetch_exception_property (obj, name, is_virtual);
    return str ? mono_string_to_utf8 (str) : NULL;
}

void
unhandled_exception_handler (MonoObject *exc, void *user_data)
{
    MonoClass *type = mono_object_get_class (exc);
    char *type_name = strdup_printf ("%s.%s", mono_class_get_namespace (type), mono_class_get_name (type));
    char *trace = mono_droid_fetch_exception_property_string (exc, "get_StackTrace", true);
    char *message = mono_droid_fetch_exception_property_string (exc, "get_Message", true);

    LOG_ERROR("UnhandledException: %s %s %s", type_name, message, trace);

    free (trace);
    free (message);
    free (type_name);
    exit (1);
}

void
log_callback (const char *log_domain, const char *log_level, const char *message, mono_bool fatal, void *user_data)
{
    LOG_INFO ("(%s %s) %s", log_domain, log_level, message);
    if (fatal) {
        LOG_ERROR ("Exit code: %d.", 1);
        exit (1);
    }
}

void runtime_init_callback ()
{
    setenv ("MONO_LOG_LEVEL", "debug", true);
    setenv ("MONO_LOG_MASK", "all", true);
#ifdef DIAGNOSTIC_PORTS
    setenv ("DOTNET_DiagnosticPorts", DIAGNOSTIC_PORTS, true);
#endif

    bool wait_for_debugger = false;
    chdir (bundle_path);

    // register bundles

    char *file_name = RUNTIMECONFIG_BIN_FILE;
    int str_len = strlen (bundle_path) + strlen (file_name) + 1; // +1 is for the "/"
    char *file_path = (char *)malloc (sizeof (char) * (str_len +1)); // +1 is for the terminating null character
    int num_char = snprintf (file_path, (str_len + 1), "%s/%s", bundle_path, file_name);
    struct stat buffer;

    LOG_INFO ("file_path: %s\n", file_path);
    assert (num_char > 0 && num_char == str_len);

    if (stat (file_path, &buffer) == 0) {
        MonovmRuntimeConfigArguments *arg = (MonovmRuntimeConfigArguments *)malloc (sizeof (MonovmRuntimeConfigArguments));
        arg->kind = 0;
        arg->runtimeconfig.name.path = file_path;
        monovm_runtimeconfig_initialize (arg, cleanup_runtime_config, file_path);
    } else {
        free (file_path);
    }

    keep_init ();

    // configure debugger support
    mono_debug_init (MONO_DEBUG_FORMAT_MONO);

    mono_install_assembly_preload_hook (mono_droid_assembly_preload_hook, NULL);
    mono_install_load_aot_data_hook (load_aot_data, free_aot_data, NULL);
    mono_install_unhandled_exception_hook (unhandled_exception_handler, NULL);
    mono_trace_set_log_handler (log_callback, NULL);

    // Signal/Crash chaining
    mono_set_signal_chaining (true);
    mono_set_crash_chaining (true);

    if (wait_for_debugger) {
        char* options[] = { "--debugger-agent=transport=dt_socket,server=y,address=0.0.0.0:55555" };
        mono_jit_parse_options (1, options);
    }

    register_aot_modules();

    mono_jit_set_aot_mode(MONO_AOT_MODE_FULL);

    MonoDomain *domain = mono_jit_init ("dotnet.android");
    // LOG_INFO ("MIHW START");
    MonoAssembly *m_embeddingLibraryAssembly = mono_assembly_open("Android.Device_Emulator.Aot_Llvm.Test.dll", NULL);
    if (!m_embeddingLibraryAssembly)
        LOG_ERROR ("Failed to load assembly: Android.Device_Emulator.Aot_Llvm.Test.dll");

    // MonoImage *m_embeddingLibraryImage = mono_assembly_get_image (m_embeddingLibraryAssembly);
    // if (!m_embeddingLibraryImage)
    //     LOG_ERROR ("Failed to get assembly image for: /data/user/0/net.dot.Android.Device_Emulator.Aot_Llvm.Test/files/Android.Device_Emulator.Aot_Llvm.Test.dll");

    // MonoClass *m_embeddingLibraryClass = mono_class_from_name (m_embeddingLibraryImage, "", "Program");
    // if (!m_embeddingLibraryClass)
    //     LOG_ERROR ("Failed to load library class: Program");

    // MonoMethod *m_embeddingLibraryClassInitializeMethod = mono_class_get_method_from_name (m_embeddingLibraryClass, "hdm", -1);
    // if (!m_embeddingLibraryClassInitializeMethod)
    //     LOG_ERROR ("Failed to get method");

    // MonoObject* result = mono_runtime_invoke (m_embeddingLibraryClassInitializeMethod, NULL, NULL, NULL);
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
