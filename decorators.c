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
    "0.0.3",
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

#define DECORS_THROW_ERROR(s)         \
    do {                              \
        zend_error(E_PARSE, "%s", s); \
        DECORS_FREE_TOKEN_ZV          \
        smart_str_free(&result);      \
    } while (0);                      \

#define DECORS_THROW_ERROR_WRONG_SYNTAX {DECORS_THROW_ERROR("wrong decorator syntax");}
#define DECORS_THROW_ERROR_MISMATCH_BRACKETS {DECORS_THROW_ERROR("mismatch paired brackets");}

/* {{{ static inline char* check_for_decor_comment(char *text, unsigned int len)
    Проверка, что текст похож на описание декоратора (регулярка ^#[ \t]*@)
    Если похож - возвращает адрес '@', а если не похож - NULL.
*/
static inline char* check_for_decor_comment(char *text, unsigned int len)
{
    if ((len<3) || ('#' != *text)) {
        return NULL;
    }

    unsigned int i;
    for (i=1; i<len; ++i) {
        if ('@' == text[i]) {
            return &(text[i]);
        }
        else if ((' ' != text[i]) && ('\t' != text[i])) {
            return NULL;
        }
    }
    return NULL;
}
/* }}} */

/* {{{ preprocessor
*/
void preprocessor(zval *source_zv, zval *return_value TSRMLS_DC)
{
    zend_lex_state  lexical_state_save;
    zval            token_zv;               // значение текущего токена
    zend_bool       token_zv_free;          // 1 - после работы с token_zv необходимо освободить занимаемую им память
    int             token_type;             // тип текущего токена (T_* или char с самим символом)
    int             parenth_nest_lvl;       // уровень вложенности парных скобок

    enum {
        STAGE_DECOR_OUTSIDE,                // вне описания декораторов и функций, ими модифицируемых
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

    zend_bool original_in_compilation = CG(in_compilation);
    CG(in_compilation) = 1;

    // Zend падает в корку, если получает NULL в качестве имени
    char *filename = zend_get_compiled_filename(TSRMLS_CC) ? zend_get_compiled_filename(TSRMLS_CC) : "";
    if (zend_prepare_string_for_scanning(source_zv, filename TSRMLS_CC) == FAILURE) {
        zend_restore_lexical_state(&lexical_state_save TSRMLS_CC);
        RETURN_FALSE;
    }

    //LANG_SCNG(yy_state) = yycINITIAL;
    LANG_SCNG(yy_state) = yycST_IN_SCRIPTING;

    ZVAL_NULL(&token_zv);
    while (token_type = lex_scan(&token_zv TSRMLS_CC)) {
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

        if ((T_COMMENT == token_type) && ((STAGE_DECOR_OUTSIDE == stage) || (STAGE_DECOR_AWAIT_FUNCTION == stage))) {
            char* decor_offs = check_for_decor_comment(zendtext, zendleng);
            if (decor_offs) { // найден комментарий, оформленный как декоратор
                smart_str_appendc(&result, '\n');

                char *end = zendtext+zendleng-1;
                char *d_name = NULL;

                ++decor_offs; // пропускаю '@', адрес которого возвращает check_for_decor_comment()
                // поиск начала имени декоратора
                while (decor_offs < end) {
                    if ((' ' != *decor_offs) && ('\t' != *decor_offs)) {
                        d_name = decor_offs;
                        break;
                    }
                    ++decor_offs;
                }

                if (!d_name) {
                    // syntax error: так и не было найдено имени декоратора после @
                    DECORS_THROW_ERROR_WRONG_SYNTAX
                    break;
                }

                // выделение имени декоратора
                while (decor_offs <= end) {
                    if (   (' '  == *decor_offs)
                        || ('\t' == *decor_offs)
                        || ('('  == *decor_offs)
                        || ('\n' == *decor_offs)
                    ) {
                        smart_str_appendl(&decor_name, d_name, decor_offs-d_name);
                        smart_str_appendc(&decor_name, '(');
                        break;
                    }
                    ++decor_offs;
                }

                // выделение параметров декоратора
                zend_bool fail_break = 0;
                while (decor_offs <= end) {
                    if ((' ' == *decor_offs) || ('\t' == *decor_offs) || ('\n' == *decor_offs)) {
                        // do nothing
                    }
                    else if ('(' == *decor_offs) {
                        char *last = memrchr(zendtext, ')', zendleng);
                        if (!last) {
                            // syntax error: открывающая '(' без парной закрывающей ')'
                            DECORS_THROW_ERROR_WRONG_SYNTAX
                            fail_break = 1;
                            break;
                        }
                        smart_str_appendl(&all_decors_params, decor_offs+1, last-decor_offs-1);
                        break;
                    }
                    else {
                        // syntax error: после имени декоратора идет непонятно что
                        DECORS_THROW_ERROR_WRONG_SYNTAX
                        fail_break = 1;
                        break;
                    }
                    ++decor_offs;
                }
                if (fail_break) {
                    break;
                }
                smart_str_appendc(&all_decors_params, DECORS_PARAMS_DELIM);

                stage = STAGE_DECOR_AWAIT_FUNCTION;

                DECORS_FREE_TOKEN_ZV
                continue;
            }
        }
        else if (STAGE_DECOR_AWAIT_FUNCTION == stage) {
            switch (token_type) {
                case T_PUBLIC:
                case T_PROTECTED:
                case T_PRIVATE:
                case T_FINAL:
                case T_ABSTRACT:
                case T_STATIC:
                case T_WHITESPACE:
                case T_COMMENT:
                case T_DOC_COMMENT:
                    break; // допустимые токены между декоратором и "function <name>"

                case T_FUNCTION:
                    stage = STAGE_DECOR_AWAIT_FUNCTION_PARAMS;
                    break;

                default:
                    // syntax error: после декоратора должна начинаться функция/метод (с опциональными комментариями)
                    DECORS_THROW_ERROR_WRONG_SYNTAX
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
                DECORS_THROW_ERROR_WRONG_SYNTAX
                break;
            }
        }
        else if (STAGE_DECOR_READ_FUNCTION_PARAMS == stage) {
            if ('(' == token_type) {
                ++parenth_nest_lvl;
            }
            else if (')' == token_type) {
                if (!parenth_nest_lvl) {
                    // syntax error: несогласованность парных скобок '(' и ')'
                    DECORS_THROW_ERROR_MISMATCH_BRACKETS
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
                stage = STAGE_DECOR_AWAIT_BODY_END;
                parenth_nest_lvl = 1;

                // сцепление имен всех вызываемых декораторов для данной функции
                // "{ return call_user_func_array(a(b(c(function(X) " + '{', которая будет выведена из zendtext
                smart_str_appends(&result, "{ return call_user_func_array(");
                smart_str_append(&result, &decor_name);
                smart_str_appends(&result, "function(");
                smart_str_append(&result, &func_args);
                smart_str_appends(&result, ") ");

                smart_str_free(&decor_name);
                smart_str_free(&func_args);
            }
            else {
                // syntax error: ошибочный или неподдерживаемый синтаксис
                DECORS_THROW_ERROR_WRONG_SYNTAX
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
                    DECORS_THROW_ERROR_MISMATCH_BRACKETS
                    break;
                }
                else {
                    if (!--parenth_nest_lvl) {
                        stage = STAGE_DECOR_OUTSIDE;

                        // "}, C)), A), func_get_args());}"
                        smart_str_appendc(&result, '}');

                        /*
                            Перевод списка параметров декораторов текущей функции в php код.
                            Список наполняется в обратном порядке, после каждой пачки параметров идет разделитель.
                            Примеры при использовании '|' в качестве разделителя:
                            0)
                                @foo(A)
                                @bar
                                @spam(B, C)
                                =>
                                'A||B, C|'
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
                                    smart_str_appendc(&result, ')');
                                    l = p;
                                }
                                else {
                                    if (l>0) {
                                        smart_str_appends(&result, ", ");
                                        smart_str_appendl(&result, s, l);
                                    }
                                    smart_str_appendc(&result, ')');
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

        smart_str_appendl(&result, zendtext, zendleng);

        DECORS_FREE_TOKEN_ZV
    }

    CG(in_compilation) = original_in_compilation;

    zend_restore_lexical_state(&lexical_state_save TSRMLS_CC);

    if (decor_name.len) {
        DECORS_THROW_ERROR("There is no function for the decorator");
    }

    smart_str_free(&decor_name);
    smart_str_free(&decor_params);
    smart_str_free(&func_args);
    smart_str_free(&all_decors_params);

    RETVAL_STRINGL(result.c, result.len, 1);
    smart_str_free(&result);
}
/* }}} */

/* {{{ DECORS_CALL_PREPROCESS */
#define DECORS_CALL_PREPROCESS(result_zv, buf, len, duplicate) \
    do {                                                       \
        zval *source_zv;                                       \
        ALLOC_INIT_ZVAL(result_zv);                            \
        ALLOC_INIT_ZVAL(source_zv);                            \
        ZVAL_STRINGL(source_zv, (buf), (len), (duplicate));    \
        preprocessor(source_zv, result_zv TSRMLS_CC);          \
        zval_dtor(source_zv);                                  \
        FREE_ZVAL(source_zv);                                  \
    } while (0);                                               \
/* }}} */

/* {{{ proto string decorators_preprocessor(string $code)
   Performs code pre-processing by replacing decorators on plain PHP code
*/
PHP_FUNCTION(decorators_preprocessor)
{
    char *source;
    int source_len;
    zval *result;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &source, &source_len) == FAILURE) {
        return;
    }

    char *filename = zend_get_compiled_filename(TSRMLS_CC) ? zend_get_compiled_filename(TSRMLS_CC) : "";
    zend_set_compiled_filename("-" TSRMLS_CC);

    DECORS_CALL_PREPROCESS(result, source, source_len, 1);

    zend_set_compiled_filename(filename TSRMLS_CC);

    RETVAL_ZVAL(result, 0, 1);
}
/* }}} */

zend_op_array* decorators_zend_compile_string(zval *source_string, char *filename TSRMLS_DC) /* {{{ */
{
    zval *result;
    DECORS_CALL_PREPROCESS(result, Z_STRVAL_P(source_string), Z_STRLEN_P(source_string), 1);

    return decorators_orig_zend_compile_string(result, filename TSRMLS_CC);
}
/* }}} */

zend_op_array* decorators_zend_compile_file(zend_file_handle *file_handle, int type TSRMLS_DC) /* {{{ */
{
    char *buf;
    size_t size;

    if (zend_stream_fixup(file_handle, &buf, &size TSRMLS_CC) == FAILURE) {
        return NULL;
    }
    // теперь в file_handle у нас гарантированно ZEND_HANDLE_MAPPED

    const char* file_path = (file_handle->opened_path) ? file_handle->opened_path : file_handle->filename;
    zend_set_compiled_filename(file_path TSRMLS_CC);

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
