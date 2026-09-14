// vm_host.h -- solo expone la tabla de callbacks del host.
// Todo el API público de la VM vive en vm.h.
#pragma once
#include "vm.h"

// ctx debe ser un FILE* -- los callbacks del host escriben eventos ahí.
extern const vm_ops_t vm_host_ops;
