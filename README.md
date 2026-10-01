# AbuMasN64

Turns SN64 cc1 assembly into GNU MIPS assembly that builds the same bytes as SN Systems ASN64 2.81.

> This is a personal tool. It is not officially maintained and may change or break without notice.

## Install

```sh
python3 -m pip install .
```

## Dependencies

- Python 3.11 or later
- GNU MIPS binutils

## Usage

```sh
abumasn64 --asn64-version 2.81 source.s > normalized.s
abumasn64 --asn64-version 2.81 source.s --run-assembler \
  --gnu-as-path "$(command -v mips-linux-gnu-as)" \
  --asflags='-march=vr4300 -mabi=32 -EB -G0 --no-pad-sections' \
  --output source.o
```

Only ASN64 2.81 is proven. Other versions are refused by name. It matches every RageWars ROM and BattleTanx US.

## Credits

Built on [RocketRet/modern-asn64](https://github.com/RocketRet/modern-asn64). See NOTICE.

## License

Released under [GNU GPL version 3 or later](LICENSE).
