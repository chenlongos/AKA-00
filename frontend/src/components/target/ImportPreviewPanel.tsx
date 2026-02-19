import { useState, useRef, useEffect } from 'react';
import type { Target } from '../../model/target';
import { renderTopDownTargets, MAP_W, MAP_H } from './TargetRenderer';
import { useTargetStore } from '../../store/targetStore';

const ImportPreviewPanel: React.FC = () => {
    const [fileContent, setFileContent] = useState<string>('');
    const [jsonText, setJsonText] = useState<string>('');
    const [parsedTargets, setParsedTargets] = useState<Target[]>([]);
    const [error, setError] = useState<string>('');
    const [isValid, setIsValid] = useState<boolean>(false);
    const canvasRef = useRef<HTMLCanvasElement>(null);
    const fileInputRef = useRef<HTMLInputElement>(null);

    const { importTargets } = useTargetStore();

    const handleFileSelect = (event: React.ChangeEvent<HTMLInputElement>) => {
        const file = event.target.files?.[0];
        if (!file) return;

        const reader = new FileReader();
        reader.onload = (e) => {
            const text = e.target?.result as string;
            setFileContent(text);
            setJsonText(text);
            validateAndParse(text);
        };
        reader.onerror = () => {
            setError('文件读取失败');
        };
        reader.readAsText(file);
    };

    const handleJsonTextChange = (event: React.ChangeEvent<HTMLTextAreaElement>) => {
        const text = event.target.value;
        setJsonText(text);
        validateAndParse(text);
    };

    const validateAndParse = (text: string) => {
        setError('');
        setIsValid(false);
        setParsedTargets([]);

        if (!text.trim()) {
            return;
        }

        try {
            const parsed = JSON.parse(text);
            if (!Array.isArray(parsed)) {
                setError('JSON数据必须是一个数组');
                return;
            }

            const validated: Target[] = [];
            for (let i = 0; i < parsed.length; i++) {
                const item = parsed[i];
                if (!item || typeof item !== 'object') {
                    setError(`第${i + 1}个元素不是有效的对象`);
                    return;
                }
                if (typeof item.id !== 'string') {
                    setError(`第${i + 1}个目标物缺少有效的id字段`);
                    return;
                }
                if (typeof item.x !== 'number' || typeof item.y !== 'number') {
                    setError(`第${i + 1}个目标物的x或y坐标不是数字`);
                    return;
                }
                if (typeof item.color !== 'string') {
                    setError(`第${i + 1}个目标物的color不是字符串`);
                    return;
                }
                if (item.type !== 'RECT' && item.type !== 'CIRCLE') {
                    setError(`第${i + 1}个目标物的type必须是"RECT"或"CIRCLE"`);
                    return;
                }
                if (item.type === 'RECT') {
                    if (item.w !== undefined && typeof item.w !== 'number') {
                        setError(`第${i + 1}个目标物的w必须是数字`);
                        return;
                    }
                    if (item.h !== undefined && typeof item.h !== 'number') {
                        setError(`第${i + 1}个目标物的h必须是数字`);
                        return;
                    }
                } else if (item.type === 'CIRCLE') {
                    if (item.r !== undefined && typeof item.r !== 'number') {
                        setError(`第${i + 1}个目标物的r必须是数字`);
                        return;
                    }
                }
                if (item.angle !== undefined && typeof item.angle !== 'number') {
                    setError(`第${i + 1}个目标物的angle必须是数字`);
                    return;
                }
                validated.push(item as Target);
            }

            setParsedTargets(validated);
            setIsValid(true);
        } catch (err) {
            setError('JSON解析错误: ' + (err instanceof Error ? err.message : String(err)));
        }
    };

    useEffect(() => {
        if (!canvasRef.current || parsedTargets.length === 0) return;
        const ctx = canvasRef.current.getContext('2d');
        if (!ctx) return;

        ctx.clearRect(0, 0, MAP_W, MAP_H);

        ctx.fillStyle = '#f9f9f9';
        ctx.fillRect(0, 0, MAP_W, MAP_H);

        ctx.strokeStyle = '#e0e0e0';
        ctx.lineWidth = 1;
        const gridSize = 50;
        for (let x = 0; x <= MAP_W; x += gridSize) {
            ctx.beginPath();
            ctx.moveTo(x, 0);
            ctx.lineTo(x, MAP_H);
            ctx.stroke();
        }
        for (let y = 0; y <= MAP_H; y += gridSize) {
            ctx.beginPath();
            ctx.moveTo(0, y);
            ctx.lineTo(MAP_W, y);
            ctx.stroke();
        }

        renderTopDownTargets(ctx, parsedTargets, null);
    }, [parsedTargets]);

    const handleConfirmImport = () => {
        if (isValid && parsedTargets.length > 0) {
            const importedCount = importTargets(parsedTargets);
            alert(`成功导入 ${importedCount} 个目标物`);
            setFileContent('');
            setJsonText('');
            setParsedTargets([]);
            setError('');
            setIsValid(false);
            if (fileInputRef.current) {
                fileInputRef.current.value = '';
            }
        }
    };

    const handleClear = () => {
        setFileContent('');
        setJsonText('');
        setParsedTargets([]);
        setError('');
        setIsValid(false);
        if (fileInputRef.current) {
            fileInputRef.current.value = '';
        }
    };

    return (
        <div>
            <h3 style={{ marginTop: 0, marginBottom: '15px' }}>数据导入与预览</h3>

            <div style={{
                display: 'flex',
                flexDirection: 'row',
                gap: '20px'
            }}>
                <div style={{
                    flex: 1,
                    display: 'flex',
                    flexDirection: 'column',
                    gap: '15px'
                }}>
                    <div>
                        <h4 style={{ marginTop: 0, marginBottom: '10px', fontSize: '14px' }}>文件导入</h4>
                        <div style={{ display: 'flex', gap: '10px', alignItems: 'center' }}>
                            <button
                                onClick={() => fileInputRef.current?.click()}
                                style={{
                                    padding: '6px 12px',
                                    fontSize: '12px',
                                    backgroundColor: '#3498db',
                                    color: 'white',
                                    border: 'none',
                                    borderRadius: '3px',
                                    cursor: 'pointer',
                                    transition: 'background-color 0.2s'
                                }}
                                onMouseEnter={(e) => e.currentTarget.style.backgroundColor = '#2980b9'}
                                onMouseLeave={(e) => e.currentTarget.style.backgroundColor = '#3498db'}
                            >
                                选择JSON文件
                            </button>
                            <input
                                ref={fileInputRef}
                                type="file"
                                accept=".json"
                                style={{ display: 'none' }}
                                onChange={handleFileSelect}
                            />
                            <span style={{ fontSize: '12px', color: '#666' }}>
                                {fileContent ? '已选择文件' : '未选择文件'}
                            </span>
                        </div>
                    </div>

                    <div>
                        <h4 style={{ marginTop: 0, marginBottom: '10px', fontSize: '14px' }}>手动输入JSON</h4>
                        <textarea
                            value={jsonText}
                            onChange={handleJsonTextChange}
                            placeholder={`输入JSON格式的目标物数组，例如：
[
  {
    "id": "target1",
    "x": 100,
    "y": 100,
    "w": 50,
    "h": 50,
    "color": "#8e44ad",
    "type": "RECT"
  }
]`}
                            style={{
                                width: '100%',
                                height: '200px',
                                padding: '10px',
                                fontSize: '12px',
                                fontFamily: 'monospace',
                                border: '1px solid #ddd',
                                borderRadius: '4px',
                                resize: 'vertical',
                                background: '#fff'
                            }}
                        />
                    </div>

                    <div>
                        {error && (
                            <div style={{
                                padding: '8px',
                                background: '#ffebee',
                                border: '1px solid #e74c3c',
                                borderRadius: '4px',
                                color: '#c0392b',
                                fontSize: '12px'
                            }}>
                                <strong>错误：</strong> {error}
                            </div>
                        )}
                        {isValid && !error && (
                            <div style={{
                                padding: '8px',
                                background: '#e8f6f3',
                                border: '1px solid #4ecdc4',
                                borderRadius: '4px',
                                color: '#16a085',
                                fontSize: '12px'
                            }}>
                                <strong>验证通过：</strong> 共 {parsedTargets.length} 个目标物
                            </div>
                        )}
                    </div>

                    <div style={{ display: 'flex', gap: '10px', marginTop: '10px' }}>
                        <button
                            onClick={handleConfirmImport}
                            disabled={!isValid}
                            style={{
                                padding: '8px 16px',
                                fontSize: '12px',
                                backgroundColor: isValid ? '#4ecdc4' : '#95a5a6',
                                color: 'white',
                                border: 'none',
                                borderRadius: '3px',
                                cursor: isValid ? 'pointer' : 'not-allowed',
                                transition: 'background-color 0.2s'
                            }}
                            onMouseEnter={(e) => {
                                if (isValid) e.currentTarget.style.backgroundColor = '#45b7d1';
                            }}
                            onMouseLeave={(e) => {
                                if (isValid) e.currentTarget.style.backgroundColor = '#4ecdc4';
                            }}
                        >
                            确认导入
                        </button>
                        <button
                            onClick={handleClear}
                            style={{
                                padding: '8px 16px',
                                fontSize: '12px',
                                backgroundColor: '#95a5a6',
                                color: 'white',
                                border: 'none',
                                borderRadius: '3px',
                                cursor: 'pointer',
                                transition: 'background-color 0.2s'
                            }}
                            onMouseEnter={(e) => e.currentTarget.style.backgroundColor = '#7f8c8d'}
                            onMouseLeave={(e) => e.currentTarget.style.backgroundColor = '#95a5a6'}
                        >
                            清空
                        </button>
                    </div>
                </div>

                <div style={{
                    flex: 1,
                    display: 'flex',
                    flexDirection: 'column',
                    gap: '10px'
                }}>
                    <h4 style={{ marginTop: 0, marginBottom: '10px', fontSize: '14px' }}>场景预览</h4>
                    <div style={{
                        border: '2px solid #333',
                        borderRadius: '4px',
                        overflow: 'hidden',
                        background: '#f9f9f9'
                    }}>
                        <canvas
                            ref={canvasRef}
                            width={MAP_W}
                            height={MAP_H}
                            style={{ display: 'block', width: '100%', height: 'auto' }}
                        />
                    </div>
                    <div style={{ fontSize: '12px', color: '#666' }}>
                        <div>预览说明：</div>
                        <ul style={{ margin: '5px 0', paddingLeft: '20px' }}>
                            <li>显示俯视视角的场景布局</li>
                            <li>矩形目标物显示为矩形，圆形目标物显示为圆形</li>
                            <li>地图尺寸: {MAP_W} × {MAP_H} 像素</li>
                        </ul>
                    </div>
                </div>
            </div>
        </div>
    );
};

export default ImportPreviewPanel;
