#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/php_smart_str.h"
#include "php_decorators.h"

#include "zend.h"
#include "zend_language_scanner.h"
#include "zend_language_scanner_defs.h"
#include "zend_language_parser.h"

#define zendtext   LANG_SCNG(yy_text)
#define zendleng   LANG_SCNG(yy_leng)
#define zendcursor LANG_SCNG(yy_cursor)
#define zendlimit  LANG_SCNG(yy_limit)

/* {{{ decorators_functions[]
 */
const zend_function_entry decorators_functions[] = {
    PHP_FE(decorators_preprocessor, NULL)
    PHP_FE_END
};
/* }}} */

/* {{{ decorators_module_entry
 */
zend_module_entry decorators_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
    STANDARD_MODULE_HEADER,
#endif
    "decorators",
    decorators_functions,
    NULL,
    NULL,
    PHP_RINIT(decorators),
    PHP_RSHUTDOWN(decorators),
    PHP_MINFO(decorators),
#if ZEND_MODULE_API_NO >= 20010901
    "0.0.1",
#endif
    STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_DECORATORS
ZEND_GET_MODULE(decorators)
#endif

zend_op_array *(*decorators_orig_zend_compile_string)(zval *source_string, char *filename TSRMLS_DC);
zend_op_array *(*decorators_orig_zend_compile_file)(zend_file_handle *file_handle, int type TSRMLS_DC);

zend_op_array* decorators_zend_compile_string(zval *source_string, char *filename TSRMLS_DC);
zend_op_array* decorators_zend_compile_file(zend_file_handle *file_handle, int type TSRMLS_DC);

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(decorators)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "decorators support", "enabled");
    php_info_print_table_end();
}
/* }}} */

/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(decorators)
{
    decorators_orig_zend_compile_string = zend_compile_string;
    zend_compile_string                 = decorators_zend_compile_string;

    decorators_orig_zend_compile_file = zend_compile_file;
    zend_compile_file                 = decorators_zend_compile_file;

    return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(decorators)
{
    zend_compile_string = decorators_orig_zend_compile_string;

    zend_compile_file = decorators_orig_zend_compile_file;

    return SUCCESS;
}
/* }}} */

#define DECORS_FREE_TOKEN_ZV                                  \
    do {                                                      \
        if (token_zv_free && (Z_TYPE(token_zv) != IS_NULL)) { \
            zval_dtor(&token_zv);                             \
        }                                                     \
        ZVAL_NULL(&token_zv);                                 \
    } while (0);                                              \

#define DECORS_THROW_ERROR(s)                               \
    do {                                                    \
        zend_throw_exception(NULL, (s), E_PARSE TSRMLS_CC); \
        DECORS_FREE_TOKEN_ZV                                \
    } while (0);                                            \

/* {{{ preprocessor(zval *source_zv, zval *return_value)
 */
void preprocessor(zval *source_zv, zval *return_value)
{
    zend_lex_state  lexical_state_save;
    zval            token_zv;               // значение текущего токена
    zend_bool       token_zv_free;          // 1 - после работы с token_zv необходимо освободить занимаемую им память
    int             token_type;             // тип текущего токена (T_* или char с самим символом)
    int             prev_token_type;        // тип предыдущего токена (T_* или char с самим символом)
    zend_bool       token_write_through;    // 1 - выводить текущий токен в результирующий код
    zend_bool       prev_token_ws_nl;       // 1 - предыдущий токен был типа T_WHITESPACE и содержал в себе \n
    int             parenth_nest_lvl;       // уровень вложенности парных скобок

    enum {
        STAGE_DECOR_OUTSIDE,                // вне описания декораторов и функций, ими модифицируемых
        STAGE_DECOR_NAME,                   // разбор имени декоратора (вызываемой функции)
        STAGE_DECOR_AWAIT_PARAMS,           // после имени декоратора ожидаю его аргументы
        STAGE_DECOR_READ_PARAMS,            // разбор аргументов декоратора
        STAGE_DECOR_AWAIT_FUNCTION,         // после описания декоратора ожидаю "function NAME"
        STAGE_DECOR_AWAIT_FUNCTION_PARAMS,  // после имени модифицируемой функции ожидаю ее аргументы
        STAGE_DECOR_READ_FUNCTION_PARAMS,   // разбор аргументов модифицируемой функции
        STAGE_DECOR_AWAIT_BODY_START,       // после аргументов вызова ожидаю начало тела функции "{"
        STAGE_DECOR_AWAIT_BODY_END          // после начала тела функции ожидаю ее конца "}"
    } stage = STAGE_DECOR_OUTSIDE;          // состояния ДКА. в начале мы заведомо вне декораторов

    smart_str   result            = {0},    // строка с результирующим PHP кодом
                decor_name        = {0},    // имя текущего декоратора (имя вызываемой функции)
                decor_params      = {0},    // параметры вызова текущего декоратора
                func_args         = {0},    // аргументы вызова текущей функции
                all_decors_params = {0};    // параметры всех декораторов текущей функции(разделены DECORS_PARAMS_DELIM)

    const char DECORS_PARAMS_DELIM = 0;     // разделитель элементов в all_decor_params

    // ОПТИМИЗАЦИЯ: если строка вообще не содержит символов "@" - возвращаю ее как есть
    if (NULL == php_memnstr(Z_STRVAL_P(source_zv), "@", 1, Z_STRVAL_P(source_zv) + Z_STRLEN_P(source_zv))) {
        RETURN_ZVAL(source_zv, 1, 0);
    }

    zend_save_lexical_state(&lexical_state_save TSRMLS_CC);

    if (zend_prepare_string_for_scanning(source_zv, "" TSRMLS_CC) == FAILURE) {
        zend_restore_lexical_state(&lexical_state_save TSRMLS_CC);
        RETURN_FALSE;
    }

    //LANG_SCNG(yy_state) = yycINITIAL;
    LANG_SCNG(yy_state) = yycST_IN_SCRIPTING;

    prev_token_ws_nl = 0;
    prev_token_type = 0;

    ZVAL_NULL(&token_zv);
    while (token_type = lex_scan(&token_zv TSRMLS_CC)) {
        token_write_through = 1;

        token_zv_free = 1;
        switch (token_type) {
            case T_CLOSE_TAG:
                if (zendtext[zendleng - 1] != '>') {
                    CG(zend_lineno)++;
                }
                // no break
            case T_OPEN_TAG:
            case T_OPEN_TAG_WITH_ECHO:
            case T_WHITESPACE:
            case T_DOC_COMMENT:
                token_zv_free = 0;
        }

        if (T_COMMENT == token_type) {
            prev_token_ws_nl = 1;
        }

        if ('@' == token_type) {
            token_write_through = 0;

            if ((STAGE_DECOR_OUTSIDE == stage) || (STAGE_DECOR_AWAIT_FUNCTION == stage)) {
                /*
                    '@' должен идти в начале строки (не считая пробельных символов) или после однострочного комментария:

                    @foo
                    // comment
                    @bar
                */
                if (prev_token_ws_nl || (T_COMMENT == prev_token_type)) {
                    stage = STAGE_DECOR_NAME;
                    DECORS_FREE_TOKEN_ZV

                    prev_token_type = token_type;

                    continue; // следующая итерация while(token_type = lex_scan)
                }
                else {
                    // syntax error: @ указан не в начале строки (перед ним допустимы только пробельные символы)
                    DECORS_THROW_ERROR("Only \\s characters accepts before @ char");
                    break;
                }
            }
            else {
                // syntax error: повторный @ внутри описания декоратора
                DECORS_THROW_ERROR("Unexpected @");
                break;
            }
        }

        prev_token_ws_nl = 0;

        if (STAGE_DECOR_OUTSIDE == stage) {
            if (T_WHITESPACE == token_type) {
                prev_token_ws_nl = (NULL != memchr(zendtext, '\n', zendleng));
            }
        }
        else if (STAGE_DECOR_NAME == stage) {
            token_write_through = 0;

            if ((T_STRING == token_type) || (T_PAAMAYIM_NEKUDOTAYIM == token_type)) {
                // все, что указано в качестве имени декоратора, сохраняю в строку
                smart_str_appendl(&decor_name, zendtext, zendleng);
            }
            else if (T_WHITESPACE == token_type) {
                prev_token_ws_nl = (NULL != memchr(zendtext, '\n', zendleng));
                if (prev_token_ws_nl) {
                    stage = STAGE_DECOR_AWAIT_FUNCTION;
                    token_write_through = 1;

                    // параметров декоратора нет - вставляю пустой блок
                    smart_str_appendc(&all_decors_params, DECORS_PARAMS_DELIM);
                }
                else {
                    stage = STAGE_DECOR_AWAIT_PARAMS;
                }
            }
            else {
                if ('(' == token_type) {
                    stage = STAGE_DECOR_READ_PARAMS;
                    parenth_nest_lvl = 1;
                }
                else {
                    // syntax error: после имени декоратора идет не пойми что
                    DECORS_THROW_ERROR("Wrong decorator syntax");
                    break;
                }
            }

            // если имя декоратора закончилось - сохраняю все накопившееся
            if (STAGE_DECOR_NAME != stage) {
                smart_str_appendc(&decor_name, '(');
            }
        }
        else if (STAGE_DECOR_AWAIT_PARAMS == stage) {
            token_write_through = 0;

            if ((T_COMMENT == token_type) || (T_DOC_COMMENT == token_type)) {
                // спокойно относимся к комментариям
                token_write_through = 1;
            }
            else if (T_WHITESPACE == token_type) {
                prev_token_ws_nl = (NULL != memchr(zendtext, '\n', zendleng));
                if (prev_token_ws_nl) {
                    stage = STAGE_DECOR_AWAIT_FUNCTION;
                    token_write_through = 1; // после имени декоратора идут пробельные символы. содержащие \n - вывожу их как есть

                    // параметров декоратора нет - вставляю пустой блок
                    smart_str_appendc(&all_decors_params, DECORS_PARAMS_DELIM);
                }
                // else // спокойно относимся к пробельным символам без начала новой строки
            }
            else if ('(' == token_type) {
                stage = STAGE_DECOR_READ_PARAMS;
                parenth_nest_lvl = 1;
            }
            else {
                // syntax error: после имени декоратора идет какая-то фигня
                DECORS_THROW_ERROR("Wrong decorator syntax");
                break;
            }
        }
        else if (STAGE_DECOR_READ_PARAMS == stage) {
            token_write_through = 0;

            if ('(' == token_type) {
                ++parenth_nest_lvl;
            }
            else if (')' == token_type) {
                if (!parenth_nest_lvl) {
                    // syntax error: несогласованность парных скобок ( и )
                    DECORS_THROW_ERROR("Mismatch paired brackets");
                    break;
                }
                else {
                    if (!--parenth_nest_lvl) {
                        stage = STAGE_DECOR_AWAIT_FUNCTION;
                    }
                }
            }

            // все, что указано в качестве параметров декоратора, сохраняю в строку
            if (STAGE_DECOR_READ_PARAMS == stage) {
                smart_str_appendl(&decor_params, zendtext, zendleng);
            }

            // если параметры закончились - сохраняю все накопившееся
            if (STAGE_DECOR_READ_PARAMS != stage) {
                smart_str_append(&all_decors_params, &decor_params);
                smart_str_appendc(&all_decors_params, DECORS_PARAMS_DELIM);
                smart_str_free(&decor_params);
            }
        }
        else if (STAGE_DECOR_AWAIT_FUNCTION == stage) {
            switch (token_type) {
                case T_WHITESPACE:
                    prev_token_ws_nl = (NULL != memchr(zendtext, '\n', zendleng));
                    // no break
                case T_PUBLIC:
                case T_PROTECTED:
                case T_PRIVATE:
                case T_FINAL:
                case T_ABSTRACT:
                case T_STATIC:
                case T_COMMENT:
                case T_DOC_COMMENT:
                    break; // допустимые токены между декоратором и "function <name>"

                case T_FUNCTION:
                    stage = STAGE_DECOR_AWAIT_FUNCTION_PARAMS;
                    break;

                default:
                    // syntax error: после декоратора должна начинаться функция/метод (с опциональными комментариями)
                    DECORS_THROW_ERROR("Wrong decorator syntax");
                    break;
            }
        }
        else if (STAGE_DECOR_AWAIT_FUNCTION_PARAMS == stage) {
            if ((T_WHITESPACE == token_type) || (T_STRING == token_type)) {
                // пробельные символы и само имя функции/метода
            }
            else if ('(' == token_type) {
                stage = STAGE_DECOR_READ_FUNCTION_PARAMS;
                parenth_nest_lvl = 1;
            }
            else {
                // syntax error: ошибочный или неподдерживаемый синтаксис
                DECORS_THROW_ERROR("Wrong decorator syntax");
                break;
            }
        }
        else if (STAGE_DECOR_READ_FUNCTION_PARAMS == stage) {
            if ('(' == token_type) {
                ++parenth_nest_lvl;
            }
            else if (')' == token_type) {
                if (!parenth_nest_lvl) {
                    // syntax error: несогласованность парных скобок ( и )
                    DECORS_THROW_ERROR("Mismatch paired brackets");
                    break;
                }
                else {
                    if (!--parenth_nest_lvl) {
                        stage = STAGE_DECOR_AWAIT_BODY_START;
                    }
                }
            }
            if (STAGE_DECOR_READ_FUNCTION_PARAMS == stage) {
                smart_str_appendl(&func_args, zendtext, zendleng);
            }
        }
        else if (STAGE_DECOR_AWAIT_BODY_START == stage) {
            if (T_WHITESPACE == token_type) {
                // пропускаю как есть
            }
            else if ('{' == token_type) {
                token_write_through = 0;

                stage = STAGE_DECOR_AWAIT_BODY_END;
                parenth_nest_lvl = 1;

                // сцепление имен всех вызываемых декораторов для данной функции
                // "{ return call_user_func_array(a(b(c(function(X) {"
                smart_str_appends(&result, "{ return call_user_func_array(");
                smart_str_append(&result, &decor_name);
                smart_str_appends(&result, "function(");
                smart_str_append(&result, &func_args);
                smart_str_appends(&result, ") { ");

                smart_str_free(&decor_name);
                smart_str_free(&func_args);
            }
            else {
                // syntax error: ошибочный или неподдерживаемый синтаксис
                DECORS_THROW_ERROR("Wrong decorator syntax");
                break;
            }
        }
        else if (STAGE_DECOR_AWAIT_BODY_END == stage) {
            if ('{' == token_type) {
                ++parenth_nest_lvl;
            }
            else if ('}' == token_type) {
                if (!parenth_nest_lvl) {
                    // syntax error: несогласованность парных скобок { и }
                    DECORS_THROW_ERROR("Mismatch paired brackets");
                    break;
                }
                else {
                    if (!--parenth_nest_lvl) {
                        stage = STAGE_DECOR_OUTSIDE;

                        // "}, C)), A), func_get_args());}"
                        smart_str_appends(&result, "}");

                        /*
                            Перевод списка параметров декораторов текущей функции в php код.
                            Список наполяется в обратном порядке, после каждой пачки параметров идет разделитель.
                            Примеры при использовании '|' в качестве разделителя:
                            0)
                                @foo(A)
                                @bar
                                @spam(B, C)
                                =>
                                'A||A, B|'
                                =>
                                foo(bar(spam(X, B, C)), A)
                            1)
                              'A|B|C|'
                              =>
                              ', C), B), A)'
                            2)
                              'A||C|'
                              =>
                              , C)), A)'
                            3)
                              '|'
                              =>
                              ')'
                            4)
                              'A1, A2|B|C1, C2|'
                              =>
                              ', C1, C2), B), A1, A2)'

                            Если у декоратора нет параметров - добавляется просто ')',
                            Если есть параметры - ', '+параметры+')'.
                        */
                        if (all_decors_params.len > 0) {
                            char *s = all_decors_params.c;
                            int   l = all_decors_params.len-1;
                            while (1) {
                                char *pp = memrchr(s, DECORS_PARAMS_DELIM, l);
                                if (pp) {
                                    int p = pp-s;
                                    int d = l-(p+1);
                                    if (d>0) {
                                        smart_str_appends(&result, ", ");
                                        smart_str_appendl(&result, s+p+1, d);
                                    }
                                    smart_str_appends(&result, ")");
                                    l = p;
                                }
                                else {
                                    if (l>0) {
                                        smart_str_appends(&result, ", ");
                                        smart_str_appendl(&result, s, l);
                                    }
                                    smart_str_appends(&result, ")");
                                    break;
                                }
                            }
                        }

                        smart_str_appends(&result, ", func_get_args());");

                        smart_str_free(&all_decors_params);
                    }
                }
            }
        }
        //else // внешний код, на который никак не влияют декораторы

        prev_token_type = token_type;

        if (token_write_through) {
            smart_str_appendl(&result, zendtext, zendleng);
        }

        DECORS_FREE_TOKEN_ZV
    }

    zend_restore_lexical_state(&lexical_state_save TSRMLS_CC);

    smart_str_free(&decor_name);
    smart_str_free(&decor_params);
    smart_str_free(&func_args);
    smart_str_free(&all_decors_params);

    RETVAL_STRINGL(result.c, result.len, 1);
    smart_str_free(&result);
} /* }}} */

#define DECORS_CALL_PREPROCESS(result_zv, buf, len, duplicate) \
    do {                                                       \
        zval *source_zv;                                       \
        ALLOC_INIT_ZVAL(result_zv);                            \
        ALLOC_INIT_ZVAL(source_zv);                            \
        ZVAL_STRINGL(source_zv, (buf), (len), (duplicate));    \
        preprocessor(source_zv, result_zv);                    \
        zval_dtor(source_zv);                                  \
        FREE_ZVAL(source_zv);                                  \
    } while (0);                                               \

/* {{{ proto string decorators_preprocessor(string $code)
   comment */
PHP_FUNCTION(decorators_preprocessor)
{
    char *source;
    int source_len;
    zval *result;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &source, &source_len) == FAILURE) {
        return;
    }

    DECORS_CALL_PREPROCESS(result, source, source_len, 0);

    RETVAL_ZVAL(result, 0, 0);
}
/* }}} */

/* {{{ decorators_zend_compile_string
   comment */
zend_op_array* decorators_zend_compile_string(zval *source_string, char *filename TSRMLS_DC)
{
    zval *result;
    DECORS_CALL_PREPROCESS(result, Z_STRVAL_P(source_string), Z_STRLEN_P(source_string), 1);

    return decorators_orig_zend_compile_string(result, filename TSRMLS_CC);
}
/* }}} */

/* {{{ decorators_zend_compile_file
   comment */
zend_op_array* decorators_zend_compile_file(zend_file_handle *file_handle, int type TSRMLS_DC)
{
    char *buf;
    size_t size;

    if (zend_stream_fixup(file_handle, &buf, &size TSRMLS_CC) == FAILURE) {
        return NULL;
    }
    // теперь в file_handle у нас гарантированно ZEND_HANDLE_MAPPED

    zval *result;
    DECORS_CALL_PREPROCESS(result, file_handle->handle.stream.mmap.buf, file_handle->handle.stream.mmap.len, 1);

    file_handle->handle.stream.mmap.buf = Z_STRVAL_P(result);
    file_handle->handle.stream.mmap.len = Z_STRLEN_P(result);

    return decorators_orig_zend_compile_file(file_handle, type TSRMLS_CC);
}
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
