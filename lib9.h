// compiler directives on plan 9
#define SET(x) ((x) = 0)
#define USED(x) ((void)(x))

// command line args
#define ARGBEGIN \
	if(argv0 == NULL) \
		argv0 = argv[0]; \
	for(argc--, argv++; \
	    *argv != NULL && argv[0][0] == '-' && argv[0][1] != '\0'; \
	    argc--, argv++) { \
		int _argc; \
		char *_tmp, *_arg = argv[0] + 1; \
		USED(_tmp); \
		if(_arg[0] == '-' && _arg[1] == '\0') { \
			argc--; \
			argv++; \
			break; \
		} \
		while((_argc = *_arg++) != '\0') \
			switch(_argc)
#define ARGC() _argc
#define ARGF() \
	(_tmp = _arg, _arg = "", \
	 (_tmp[0] != '\0') ? _tmp : (argv[1] != NULL) ? (argc--, *++argv) : 0)
#define EARGF(x)                 \
	(_tmp = _arg, _arg = "", \
	 (*_tmp ? _tmp : argv[1] ? (argc--, *++argv) : ((x), abort(), (char *)0)))
#define ARGEND \
	} \
	USED(argv); \
	USED(argc);
