{
  description = "M125 test fixture: a flake with no declared inputs";
  outputs = { self }: {
    packages.x86_64-linux.hello = "hello-value";
    selfOutPath = self.outPath;
    selfKeys = builtins.attrNames self;
    selfType = self._type;
  };
}
