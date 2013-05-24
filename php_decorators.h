#ifndef PHP_DECORATORS_H
#define PHP_DECORATORS_H

extern zend_module_entry decorators_module_entry;
#define phpext_decorators_ptr &decorators_module_entry

#ifdef PHP_WIN32
#   define PHP_DECORATORS_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#   define PHP_DECORATORS_API __attribute__ ((visibility("default")))
#else
#   define PHP_DECORATORS_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

PHP_RINIT_FUNCTION(decorators);
PHP_RSHUTDOWN_FUNCTION(decorators);
PHP_MINFO_FUNCTION(decorators);

PHP_FUNCTION(decorators_preprocessor);

#endif  /* PHP_DECORATORS_H */
