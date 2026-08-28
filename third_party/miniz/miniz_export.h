#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H

/* Static-only vendored mirror of GenerateExportHeader output: every export
   macro is empty because layerc_miniz is never built as a DLL. */
#define MINIZ_EXPORT
#define MINIZ_NO_EXPORT
#define MINIZ_DEPRECATED __declspec(deprecated)
#define MINIZ_DEPRECATED_EXPORT MINIZ_DEPRECATED
#define MINIZ_DEPRECATED_NO_EXPORT MINIZ_DEPRECATED

#endif /* MINIZ_EXPORT_H */