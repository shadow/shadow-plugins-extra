/*
 * See LICENSE for licensing information
 */

#include "python-plugin.h"
#include "python-global-lock.h"


/*
 * This is actually a pretty dirty hack: The argv0 required here
 * cannot be the argv[0] of the plugin as it is in the wrong path.
 * Then Python will not search in the correct path for the modules.
 * Thus, we make this a static string based on the prefix set during
 * compilation of the plugin. Shouldn't cause too much harm but would
 * like to see a better solution.
 */
#if PY_MAJOR_VERSION >= 3
#define Lstr2(s) L ## s
#define Lstr(s) Lstr2(s)
#define ARGV0 Lstr(INSTALL_PREFIX) L"/bin/shadow-python3"
#else
#define ARGV0 INSTALL_PREFIX "/bin/shadow-python"
#endif

#if PY_MAJOR_VERSION >= 3
int _PyImport_FixupExtensionObject(PyObject *mod, PyObject *name, PyObject *filename);
#endif

static PyObject *prepare_interpreter(int argc, char *argv[], python_data *m) {
    if(argc < 2) {
        PyErr_SetString(PyExc_RuntimeError, "Need a file");
        return NULL;   
    }

    /* Create sub-interpreter */
    _py_log(G_LOG_LEVEL_MESSAGE, __FUNCTION__, "starting new interpreter");
    m->interpreter = Py_NewInterpreter();
    if(!m->interpreter)
        return NULL;

#if PY_MAJOR_VERSION >= 3
    /* On Python 3 we need to convert them to wchar's */
    wchar_t **argv_w = calloc(argc, sizeof(wchar_t *));
    if(!argv_w) {
        PyErr_SetString(PyExc_RuntimeError, "Out of memory");
        return NULL;
    }

    int i;
    for(i = 0; i < argc; i++) {
        int req_size = mbstowcs(NULL, argv[i], 0) + 1;
        if(req_size <= 1) {
            PyErr_SetString(PyExc_RuntimeError, "Can't determine arg size");
            return NULL;
        }
        argv_w[i] = calloc(req_size, sizeof(wchar_t));
        if(!argv_w[i]) {
            PyErr_SetString(PyExc_RuntimeError, "Out of memory");
            return NULL;
        }
        if(mbstowcs(argv_w[i], argv[i], req_size) <= 0) {
            PyErr_SetString(PyExc_RuntimeError, "Can't convert char to wchar");
            return NULL;
        }
    }

    /* Now we can set argv_w */
    PySys_SetArgv(argc-1, argv_w+1);

    /* Clean up acquired memory */
    for(i = 0; i < argc; i ++) {
        free(argv_w[i]);
    }
    free(argv_w);
    PyObject *argv0 = PyUnicode_FromString(argv[1]);
#else
    PySys_SetArgv(argc-1, argv+1);
    PyObject *argv0 = PyString_FromString(argv[1]);
#endif
    if(!argv0)
        return NULL;

    PyObject *split_result = PyObject_CallMethod(argv0, "rsplit", "(si)", "/", 1);
    Py_DECREF(argv0);
    Py_ssize_t split_size = PyList_Size(split_result);
    if(!split_result || split_size > 2 || split_size < 1) {
        Py_XDECREF(split_result);
        return NULL;
    }
    PyObject *path, *mod_filename;
    if(split_size == 1) {
#if PY_MAJOR_VERSION >= 3
        path = PyUnicode_FromString(".");
#else
        path = PyString_FromString(".");
#endif
        mod_filename = PyList_GetItem(split_result, 0);
    } else {
        /* split_size == 2 */
        path = PyList_GetItem(split_result, 0);
        if(path)
            Py_INCREF(path);
        mod_filename = PyList_GetItem(split_result, 1);
    }
    if(mod_filename)
        Py_INCREF(mod_filename);
    Py_DECREF(split_result);
    if(!mod_filename || !path) {
        Py_XDECREF(mod_filename);
        Py_XDECREF(path);
        return NULL;
    }

    PyObject *sys_path = PySys_GetObject("path");
    if(!sys_path) {
        Py_DECREF(mod_filename);
        Py_DECREF(path);
        return NULL;
    }

    if(PyList_SetItem(sys_path, 0, path) != 0) {
        Py_DECREF(mod_filename);
        Py_DECREF(path);
        return NULL;
    }

    split_result = PyObject_CallMethod(mod_filename, "rsplit", "(si)", ".", 1);
    Py_DECREF(mod_filename);
    if(!split_result)
        return NULL;
    PyObject *module_name = PyList_GetItem(split_result, 0);
    Py_INCREF(module_name);
    Py_DECREF(split_result);
    return module_name;
}


python_data *python_new(int argc, char *argv[]) {
    PyObject *get_handle = NULL, *args = NULL, *module_name;
    python_data* m = NULL;
    PyThreadState *saved_tstate;
    int error = 0;

#define PYERR()                 \
    do {                               \
        PyErr_Print();                 \
        error = 1; \
        goto err;                      \
    } while(0)

    shadow_python_lock();
    _py_log(G_LOG_LEVEL_MESSAGE, __FUNCTION__, "python_new called");
    /* See the comments on the definition of ARGV0 on why this is necessary */
#if PY_MAJOR_VERSION >= 3
    wchar_t *argv_0 = wcsdup(ARGV0);
#else
    char *argv_0 = strdup(ARGV0);
#endif
    assert(argv_0);
    /* Must be the first thing we do to get everything else started */
    Py_SetProgramName(argv_0);
    Py_Initialize();

    /* This code prevents a segfault if too many interpreter instances
     * are started. I know we have an error somewhere but I have no idea
     * where and so we need to hack it with this.
     */
    PyObject *gc = PyImport_ImportModule("gc");
    assert(gc);
    Py_DECREF(PyObject_CallMethod(gc, "disable", NULL));


    /* We start a new sub interpreter, so we back the old one up to 
     * restore it later
     */
    saved_tstate = PyThreadState_Get();

    m = calloc(1, sizeof(python_data));
    assert(m);

    if(!(module_name = prepare_interpreter(argc, argv, m)))
         PYERR();

    PyObject *shd_py = init_logger();
    if(!shd_py)
        PYERR();
#if PY_MAJOR_VERSION >= 3
    PyObject *name = PyUnicode_FromString("shadow_python");
    /* Remember pointer to module init function. */
    if (_PyImport_FixupExtensionObject(shd_py, name, name) < 0) {
        Py_DECREF(shd_py);
        PYERR();
    }
    /* FixupExtension has put the module into sys.modules,
       so we can release our own reference. */
    Py_DECREF(shd_py);
#endif

    m->module = PyImport_Import(module_name);
    if(m->module == NULL)
        PYERR();
    get_handle = PyObject_GetAttrString(m->module, "get_handle");
    if(get_handle == NULL)
        PYERR();

    args = PyTuple_New(0);
    Py_INCREF(Py_None);
    if(args == NULL) {
        PYERR();
    }

    m->handle = PyObject_Call(get_handle, args, NULL);
    if(m->handle == NULL)
        PYERR();

    m->process = PyObject_GetAttrString(m->handle, "process");
    if(m->process == NULL)
        PYERR();

    m->finish = PyObject_GetAttrString(m->handle, "finish");
    if(m->finish == NULL)
        PYERR();
err:
    Py_XDECREF(get_handle);
    Py_XDECREF(args);
    Py_XDECREF(module_name);
    PyThreadState_Swap(saved_tstate);
    shadow_python_unlock();
    if(error) {
        python_free(m);
        m = NULL;
    }
    return m;
#undef PYERR
}

int python_ready(python_data *m) {
    shadow_python_lock();

    /* we need to switch to our interpreter */
    PyThreadState *saved_tstate = PyThreadState_Swap(m->interpreter);
    _py_log(G_LOG_LEVEL_DEBUG, __FUNCTION__, "python_ready called, interpreter %p, saved: %p", m->interpreter, saved_tstate);

    PyObject *args = PyTuple_New(0), *retval = NULL;
    if(args == NULL) {
        PyErr_Print();
        shadow_python_unlock();
        return 1;
    }
    retval = PyObject_Call(m->process, args, NULL);
    if(retval == NULL) {
        PyErr_Print();
        Py_XDECREF(args);
        PyThreadState_Swap(saved_tstate);
        _py_log(G_LOG_LEVEL_ERROR, __FUNCTION__, "Unexpected return during process, aborting");
        shadow_python_unlock();
        return 1;
    }
    int retint = PyObject_IsTrue(retval);
    Py_XDECREF(args);
    Py_XDECREF(retval);
    saved_tstate = PyThreadState_Swap(saved_tstate);
    assert(saved_tstate == m->interpreter);
    _py_log(G_LOG_LEVEL_DEBUG, __FUNCTION__, "python_ready finished, interpreter %p, saved: %p", m->interpreter, saved_tstate);
    shadow_python_unlock();
    return retint;
}

void python_free(python_data *m) {
    if(m != NULL) {
        shadow_python_lock();
        _py_log(G_LOG_LEVEL_DEBUG, __FUNCTION__, "python_free called");
        /* we need to switch to our interpreter */
        PyThreadState *saved_tstate = PyThreadState_Get();
        if(m->interpreter)
            PyThreadState_Swap(m->interpreter);

        /* call finish function if it was started */
        if(m->finish) {
            PyObject *args = PyTuple_New(0);
            if(args != NULL) {
                if(PyObject_Call(m->finish, args, NULL) == NULL)
                    PyErr_Print();
                Py_XDECREF(args);
                Py_XDECREF(m->finish);   
            } else {
                PyErr_Print();
            }
        }
        Py_XDECREF(m->process);
        Py_XDECREF(m->handle);
        Py_XDECREF(m->module);

        if(m->interpreter)
            Py_EndInterpreter(m->interpreter);
        PyThreadState_Swap(saved_tstate);
        free(m);
        shadow_python_unlock();
    }
}