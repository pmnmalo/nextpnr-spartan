export type Tree = {
    [name: string]: Tree | string | Uint8Array
};

export type InputStream =
    (byteLength: number) => Uint8Array | null;

export type OutputStream =
    (bytes: Uint8Array | null) => void;

export type ProgressCallback =
    (event: { source: Application, totalLength: number, doneLength: number }) => void;

export type RunOptions = {
    stdin?:  InputStream  | null;
    stdout?: OutputStream | null;
    stderr?: OutputStream | null;
    decodeASCII?: boolean;
    synchronously?: boolean;
    fetchProgress?: ProgressCallback;
};

export class Application {
    argv0: string;

    constructor(resources: () => Promise<any>, instantiate: any, argv0: string);

    run(args?: string[], files?: Tree, options?: RunOptions): Promise<Tree> | Tree | undefined;
}

export class Exit extends Error {
    code: number;
    files: Tree;
}

//--------8<--------8<--------8<--------8<--------8<--------8<--------8<--------8<--------8<--------

export type Command =
    (args?: string[], files?: Tree, options?: RunOptions) => Promise<Tree> | Tree | undefined;

export const runNextpnrHimbaechelSpartan3e: Command;

export const commands: {
    'nextpnr-himbaechel-spartan3e': Command,
};

export const version: string;

/** The devices whose chip databases the package has (xc3s100e, xc3s250e, xc3s500e, xc3s1200e). */
export const devices: string[];

/** The device of a part name (xc3s250ecp132-4 -> xc3s250e); throws for a device without a chip database. */
export function deviceOf(part: string): string;

/** URL of a part's chip database file (gzipped), beside the package's bundle. */
export function chipdbUrl(part: string): URL;

/** A part's chip database, decompressed: give it to nextpnr as a file, with --chipdb. */
export function chipdb(part: string): Promise<Uint8Array>;
