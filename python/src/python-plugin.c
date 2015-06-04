#include "python-plugin.h"
#include <gmodule.h>
/* functions that interface into shadow */
ShadowFunctionTable shadowlib;

/* our opaque instance of the python node */
python_data* pyInstance = NULL;

/* shadow is notifying us that some descriptors are ready to read/write */
static void pythonplugin_ready() {
    /* shadow wants to handle some descriptor I/O. pass this to the lower level
     * plug-in function that implements this for both plug-in and non-plug-in modes.
     */
    // pyInstance->log(SHADOW_LOG_LEVEL_DEBUG, __FUNCTION__, "python_ready called");
    python_ready(pyInstance);
}

static void pythonplugin_periodic_cb(void *data) {
    pythonplugin_ready();
    shadowlib.createCallback(&pythonplugin_periodic_cb, NULL, 100);
}

/* shadow is creating a new instance of this plug-in as a node in
 * the simulation. argc and argv are as configured via the XML.
 */
static void pythonplugin_new(int argc, char* argv[]) {
    /* shadow wants to create a new node. pass this to the lower level
     * plug-in function that implements this for both plug-in and non-plug-in modes.
     * also pass along the interface shadow gave us earlier.
     *
     * the value of pyInstance will be different for every node, because
     * we did not set it in __shadow_plugin_init__(). this is desirable, because
     * each node needs its own application state.
     */
    pyInstance = python_new(argc, argv, shadowlib.log);

    /*
     * Add a callback to be called periodically
     * This calls the ready function regularly, even when no events happened
     */
    shadowlib.createCallback(&pythonplugin_periodic_cb, NULL, 100);
}

/* shadow is freeing an existing instance of this plug-in that we previously
 * created in pythonplugin_new()
 */
static void pythonplugin_free() {
    /* shadow wants to free a node. pass this to the lower level
     * plug-in function that implements this for both plug-in and non-plug-in modes.
     */
    python_free(pyInstance);
}

/* plug-in initialization. this only happens once per plug-in,
 * no matter how many nodes (instances of the plug-in) are configured.
 *
 * whatever state is configured in this function will become the default
 * starting state for each node.
 *
 * the "__shadow_plugin_init__" function MUST exist in every plug-in.
 */
void __shadow_plugin_init__(ShadowFunctionTable* shadowlibFuncs) {
    assert(shadowlibFuncs);

    /* locally store the functions we use to call back into shadow */
    shadowlib = *shadowlibFuncs;

    /*
     * tell shadow how to call us back when creating/freeing nodes, and
     * where to call to notify us when there is descriptor I/O
     */
    int success = shadowlib.registerPlugin(&pythonplugin_new, &pythonplugin_free, &pythonplugin_ready);

    /* we log through Shadow by using the log function it supplied to us */
    if(success) {
        shadowlib.log(SHADOW_LOG_LEVEL_MESSAGE, __FUNCTION__,
                "successfully registered python plug-in state");
    } else {
        shadowlib.log(SHADOW_LOG_LEVEL_CRITICAL, __FUNCTION__,
                "error registering python plug-in state");
    }
}

/* called immediately after the plugin is unloaded. shadow unloads plugins
 * once for each worker thread.
 */
void g_module_unload(GModule *module) {
    Py_Finalize();
}