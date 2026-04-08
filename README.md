# Sindarin YAML

A zero-dependency YAML encoder/decoder for the [Sindarin](https://github.com/SindarinSDK/sindarin-compiler) programming language. Provides fast YAML serialization and deserialization for `@serializable` structs using direct string building — no external libraries required.

## Installation

Add the package as a dependency in your `sn.yaml`:

```yaml
dependencies:
- name: sindarin-pkg-yaml
  git: git@github.com:SindarinSDK/sindarin-pkg-yaml.git
  branch: main
```

Then run `sn --install` to fetch the package.

## Quick Start

Define a `@serializable` struct and encode/decode it:

```sindarin
import "yaml/yaml"

@serializable
struct Person =>
    name: str
    age: int
    active: bool

fn main(): void =>
    // Encode
    var p: Person = Person { name: "Alice", age: 30, active: true }
    var enc: Encoder = Yaml.encoder()
    p.encode(enc)
    var yamlStr: str = enc.result()
    println(yamlStr)
    // name: "Alice"
    // age: 30
    // active: true

    // Decode
    var p2: Person = Person.decode(Yaml.decoder(yamlStr))
    println(p2.name)   // Alice
```

## Documentation

### Module

| Module | Import | Description |
|--------|--------|-------------|
| Yaml | `import "yaml/yaml"` | YAML encoder/decoder for `@serializable` structs |

### Yaml

Static factory methods for creating encoders and decoders.

```sindarin
import "yaml/yaml"
```

#### Encoding

```sindarin
Yaml.encoder(): Encoder          # Create an object encoder (produces block-style YAML)
Yaml.arrayEncoder(): Encoder     # Create an array encoder (produces "- item" block sequence)
```

The returned `Encoder` is passed to a struct's generated `.encode()` method. Call `.result()` on the encoder to get the final YAML string.

```sindarin
@serializable
struct Address =>
    street: str
    city: str

var a: Address = Address { street: "123 Main St", city: "NYC" }
var enc: Encoder = Yaml.encoder()
a.encode(enc)
var yaml: str = enc.result()
// street: "123 Main St"
// city: "NYC"
```

#### Decoding

```sindarin
Yaml.decoder(input: str): Decoder        # Parse a YAML string (object or array)
Yaml.arrayDecoder(input: str): Decoder   # Alias for decoder
```

The returned `Decoder` is passed to a struct's generated `.decode()` static method.

```sindarin
var dec: Decoder = Yaml.decoder("street: \"123 Main St\"\ncity: \"NYC\"")
var a: Address = Address.decode(dec)
println(a.street)  // 123 Main St
```

### Supported Types

The encoder/decoder handles all types supported by `@serializable`:

| Type | YAML Representation |
|------|---------------------|
| `str` | `"string"` (always double-quoted) |
| `int` | `123` |
| `double` | `9.5` |
| `bool` | `true` / `false` |
| Nested structs | indented `key: value` lines |
| Arrays (`T[]`) | `- item` block sequence |

String values are always double-quoted with JSON-style escaping (quotes, newlines, tabs, etc.) during encoding and unescaped during decoding. This keeps the format roundtrip-safe regardless of string content.

### Emission Format

This package emits **block-style YAML** with the following rules:

- Strings are always double-quoted (never bare scalars)
- Nested objects/arrays indent by **2 spaces** per level
- Empty objects emit `{}`, empty arrays emit `[]`
- NaN and infinite doubles are emitted as `null`

The decoder handles the format the encoder produces (closed-loop roundtrip). It is **not** a general-purpose YAML 1.2 parser — features like anchors, aliases, block scalars (`|`, `>`), tags, and multi-document streams are not supported.

### Nested Structs and Arrays

`@serializable` structs can contain other `@serializable` structs and arrays:

```sindarin
@serializable
struct Address =>
    street: str
    city: str

@serializable
struct Person =>
    name: str
    age: int
    address: Address
    tags: str[]

@serializable
struct Team =>
    name: str
    members: Person[]

var team: Team = Team {
    name: "Engineering",
    members: {
        Person { name: "Alice", age: 30, address: Address { street: "1 A", city: "X" }, tags: {"dev"} },
        Person { name: "Bob", age: 25, address: Address { street: "2 B", city: "Y" }, tags: {"ops", "sre"} }
    }
}

var enc: Encoder = Yaml.encoder()
team.encode(enc)
var yaml: str = enc.result()

// Roundtrip back to a struct
var team2: Team = Team.decode(Yaml.decoder(yaml))
```

## Development

### Running Tests

```bash
make test
```

### Available Targets

```bash
make test       # Run tests
make clean      # Remove build artifacts
make help       # Show all targets
```

## Dependencies

This package depends on [sindarin-pkg-test](https://github.com/SindarinSDK/sindarin-pkg-test) for the test runner. Dependencies are automatically managed via the `sn.yaml` package manifest.

## License

MIT License
