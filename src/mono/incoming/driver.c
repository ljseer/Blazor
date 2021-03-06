#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef enum {
	/* Disables AOT mode */
	MONO_AOT_MODE_NONE,
	/* Enables normal AOT mode, equivalent to mono_jit_set_aot_only (false) */
	MONO_AOT_MODE_NORMAL,
	/* Enables hybrid AOT mode, JIT can still be used for wrappers */
	MONO_AOT_MODE_HYBRID,
	/* Enables full AOT mode, JIT is disabled and not allowed,
	 * equivalent to mono_jit_set_aot_only (true) */
	MONO_AOT_MODE_FULL,
	/* Same as full, but use only llvm compiled code */
	MONO_AOT_MODE_LLVMONLY,
	/* Uses Interpreter, JIT is disabled and not allowed,
	 * equivalent to "--full-aot --interpreter" */
	MONO_AOT_MODE_INTERP,
	/* Same as INTERP, but use only llvm compiled code */
	MONO_AOT_MODE_INTERP_LLVMONLY,
} MonoAotMode;

typedef enum {
	MONO_IMAGE_OK,
	MONO_IMAGE_ERROR_ERRNO,
	MONO_IMAGE_MISSING_ASSEMBLYREF,
	MONO_IMAGE_IMAGE_INVALID
} MonoImageOpenStatus;

typedef struct MonoDomain_ MonoDomain;
typedef struct MonoAssembly_ MonoAssembly;
typedef struct MonoMethod_ MonoMethod;
typedef struct MonoException_ MonoException;
typedef struct MonoString_ MonoString;
typedef struct MonoClass_ MonoClass;
typedef struct MonoImage_ MonoImage;
typedef struct MonoObject_ MonoObject;
typedef struct MonoThread_ MonoThread;
typedef struct _MonoAssemblyName MonoAssemblyName;


void mono_jit_set_aot_mode (MonoAotMode mode);
MonoDomain*  mono_jit_init_version (const char *root_domain_name, const char *runtime_version);
MonoAssembly* mono_assembly_open (const char *filename, MonoImageOpenStatus *status);
int mono_jit_exec (MonoDomain *domain, MonoAssembly *assembly, int argc, char *argv[]);
void mono_set_assemblies_path (const char* path);
int monoeg_g_setenv(const char *variable, const char *value, int overwrite);
void mono_free (void*);
MonoString* mono_string_new (MonoDomain *domain, const char *text);
MonoDomain* mono_domain_get (void);
MonoClass* mono_class_from_name (MonoImage *image, const char* name_space, const char *name);
MonoMethod* mono_class_get_method_from_name (MonoClass *klass, const char *name, int param_count);

MonoString* mono_object_to_string (MonoObject *obj, MonoObject **exc);//FIXME Use MonoError variant
char* mono_string_to_utf8 (MonoString *string_obj);
MonoObject* mono_runtime_invoke (MonoMethod *method, void *obj, void **params, MonoObject **exc);
MonoImage* mono_assembly_get_image (MonoAssembly *assembly);
MonoAssembly* mono_assembly_load (MonoAssemblyName *aname, const char *basedir, MonoImageOpenStatus *status);

MonoAssemblyName* mono_assembly_name_new (const char *name);
void mono_assembly_name_free (MonoAssemblyName *aname);
const char* mono_image_get_name (MonoImage *image);
const char* mono_class_get_name (MonoClass *klass);
MonoString* mono_string_new (MonoDomain *domain, const char *text);
void mono_add_internal_call (const char *name, const void* method);
MonoString * mono_string_from_utf16 (char *data);
MonoString* mono_string_new (MonoDomain *domain, const char *text);


static char*
m_strdup (const char *str)
{
	if (!str)
		return NULL;

	int len = strlen (str) + 1;
	char *res = malloc (len);
	memcpy (res, str, len);
	return res;
}

static MonoDomain *root_domain;

static void*
mono_wasm_invoke_js (MonoString **exceptionMessage, MonoString *funcName, void* arg0, void* arg1, void* arg2)
{
	*exceptionMessage = NULL;
	char *funcNameUtf8 = mono_string_to_utf8 (funcName);
	void *jsCallResult = (void *)EM_ASM_INT ({
		try {
			// Get the function you're trying to invoke
			var funcNameJsString = UTF8ToString ($1);
			var blazorExports = window.Blazor;
			if (!blazorExports) { // Shouldn't be possible, because you can't start up the .NET code without loading that library
				throw new Error('The Blazor JavaScript library is not loaded.');
			}
			var funcInstance = blazorExports.platform.monoGetRegisteredFunction(funcNameJsString);

			return funcInstance.call(null, $2, $3, $4);
		} catch (ex) {
			var exceptionJsString = ex.message + '\n' + ex.stack;
			var mono_string = Module.cwrap('mono_wasm_string_from_js', 'number', ['string']); // TODO: Cache
			var exceptionSystemString = mono_string(exceptionJsString);
			setValue ($0, exceptionSystemString, 'i32'); // *exceptionMessage = exceptionSystemString;
			return 0;
		}
	}, exceptionMessage, (int)funcNameUtf8, (int)arg0, (int)arg1, (int)arg2);
	mono_free (funcNameUtf8);

	return jsCallResult;
}

static void*
mono_wasm_invoke_js_array (MonoString **exceptionMessage, MonoString *funcName, MonoObject *argsArray)
{
	*exceptionMessage = NULL;
	char *funcNameUtf8 = mono_string_to_utf8 (funcName);
	void *jsCallResult = (void *)EM_ASM_INT ({
		try {
			// Get the function you're trying to invoke
			var funcNameJsString = UTF8ToString ($0);
			var blazorExports = window.Blazor;
			if (!blazorExports) { // Shouldn't be possible, because you can't start up the .NET code without loading that library
				throw new Error('The Blazor JavaScript library is not loaded.');
			}
			var funcInstance = blazorExports.platform.monoGetRegisteredFunction(funcNameJsString);
			
			// Map the incoming .NET object array to a JavaScript array of System_Object pointers
			var argsArrayDataPtr = $1 + 12; // First byte from here is length, then following bytes are entries
			var argsArrayLength = Module.getValue(argsArrayDataPtr, 'i32');
			var argsJsArray = [];
			for (var i = 0; i < argsArrayLength; i++) {
				argsArrayDataPtr += 4;
				argsJsArray[i] = Module.getValue(argsArrayDataPtr, 'i32');
			}

			return funcInstance.apply(null, argsJsArray);
		} catch (ex) {
			var exceptionJsString = ex.message + '\n' + ex.stack;
			var mono_string = Module.cwrap('mono_wasm_string_from_js', 'number', ['string']); // TODO: Cache
			var exceptionSystemString = mono_string(exceptionJsString);
			setValue ($2, exceptionSystemString, 'i32'); // *exceptionMessage = exceptionSystemString;
			return 0;
		}
	}, (int)funcNameUtf8, (int)argsArray, exceptionMessage);
	mono_free (funcNameUtf8);

	return jsCallResult;
}

EMSCRIPTEN_KEEPALIVE void
mono_wasm_load_runtime (const char *managed_path)
{
	monoeg_g_setenv ("MONO_LOG_LEVEL", "debug", 1);
	monoeg_g_setenv ("MONO_LOG_MASK", "gc", 1);
	mono_jit_set_aot_mode (MONO_AOT_MODE_INTERP_LLVMONLY);
	mono_set_assemblies_path (m_strdup (managed_path));
	root_domain = mono_jit_init_version ("mono", "v4.0.30319");

	mono_add_internal_call ("WebAssembly.Runtime::InvokeJS", mono_wasm_invoke_js);
	mono_add_internal_call ("WebAssembly.Runtime::InvokeJSArray", mono_wasm_invoke_js_array);
}

EMSCRIPTEN_KEEPALIVE MonoAssembly*
mono_wasm_assembly_load (const char *name)
{
	MonoImageOpenStatus status;
	MonoAssemblyName* aname = mono_assembly_name_new (name);
	if (!name)
		return NULL;

	MonoAssembly *res = mono_assembly_load (aname, NULL, &status);
	mono_assembly_name_free (aname);

	return res;
}

EMSCRIPTEN_KEEPALIVE MonoClass*
mono_wasm_assembly_find_class (MonoAssembly *assembly, const char *namespace, const char *name)
{
	return mono_class_from_name (mono_assembly_get_image (assembly), namespace, name);
}

EMSCRIPTEN_KEEPALIVE MonoMethod*
mono_wasm_assembly_find_method (MonoClass *klass, const char *name, int arguments)
{
	return mono_class_get_method_from_name (klass, name, arguments);
}

EMSCRIPTEN_KEEPALIVE MonoObject*
mono_wasm_invoke_method (MonoMethod *method, MonoObject *this_arg, void *params[], int* got_exception)
{
	MonoObject *exc = NULL;
	MonoObject *res = mono_runtime_invoke (method, this_arg, params, &exc);
	*got_exception = 0;

	if (exc) {
		*got_exception = 1;

		MonoObject *exc2 = NULL;
		res = (MonoObject*)mono_object_to_string (exc, &exc2); 
		if (exc2)
			res = (MonoObject*) mono_string_new (root_domain, "Exception Double Fault");
		return res;
	}

	return res;
}

EMSCRIPTEN_KEEPALIVE char *
mono_wasm_string_get_utf8 (MonoString *str)
{
	return mono_string_to_utf8 (str); //XXX JS is responsible for freeing this
}

EMSCRIPTEN_KEEPALIVE MonoString *
mono_wasm_string_from_js (const char *str)
{
	return mono_string_new (root_domain, str);
}
