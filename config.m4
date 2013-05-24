dnl $Id$
dnl config.m4 for extension decorators

PHP_ARG_ENABLE(decorators, whether to enable decorators support,
[  --enable-decorators           Enable decorators support])

if test "$PHP_DECORATORS" != "no"; then
  PHP_NEW_EXTENSION(decorators, decorators.c, $ext_shared)
fi
