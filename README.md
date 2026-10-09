# AbuMasN64

Builds `n64link`, a small program whose `asn64` step turns SN64 cc1 assembly into GNU MIPS assembly that builds the same bytes as SN Systems ASN64 2.81.

> This is a personal tool. It is not officially maintained and may change or break without notice.

## Build

```sh
make
```

This writes the static binary `build/n64link`. It needs a C compiler with a static libc.

## Usage

```sh
n64link asn64 [SOURCE.s]
n64link asn64 --as mips-linux-gnu-as -march=vr4300 -mabi=32 -EB -G0 --no-pad-sections source.s -o source.o
```

The first form writes normalised assembly to stdout. The second assembles it with GNU as. Only ASN64 2.81 is proven. It builds every RageWars ROM and BattleTanx US byte for byte.

## Development

Run `ci/check` with `N64LINK_GNU_AS` set to GNU MIPS as.

## Credits

The asn64 rules come from [RocketRet/modern-asn64](https://github.com/RocketRet/modern-asn64). See NOTICE.

## License

Released under [GNU GPL version 3 or later](LICENSE).
